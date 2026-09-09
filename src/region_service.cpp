#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wininet.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <climits>
#include "region_service.h"
#include "ui.h"
std::vector<Region> g_regions;
int REGION_COUNT = 0;

static const char*  HOSTS_PATH  = "C:\\Windows\\System32\\drivers\\etc\\hosts";
static const char*  HOSTS_DIR   = "C:\\Windows\\System32\\drivers\\etc";
static const char*  HOST_PREFIX = "gamelift-ping.";
static const char*  HOST_SUFFIX = ".api.aws";
static const size_t MAX_HTTP_BODY = 64 * 1024;
static const wchar_t* QUEUES_URL = L"https://api2.deadbyqueue.com/queues";
static const wchar_t* REGIONS_URL = L"https://api2.deadbyqueue.com/regions";
static const char* REGION_CACHE_NAME = "regions.cache";

HWND g_hList = nullptr, g_hBtnRefresh = nullptr, g_hBtnApply = nullptr, g_hwndMain = nullptr;
HFONT g_fontUI = nullptr, g_fontBold = nullptr;
std::vector<bool> g_unblockedRegions;
LONG g_pendingFetches = 0;
LONG g_completedFetches = 0;
LONG g_activeWorkers = 0;
bool g_refreshing = false;
bool g_updatingList = false;
bool g_wsaStarted = false;
bool g_hasUnappliedChanges = false; // Tracks unapplied changes
std::vector<bool> g_originalUnblockedRegions;
int g_spinnerFrame = 0;
bool g_applyFlash = false;
std::vector<int> g_order;   // display row -> region index, current sort order
int g_sortColumn = 0;       // 0=region, 1=killer, 2=survivor, 3=ping
bool g_sortAscending = true;
HWND g_hHeader = nullptr;
HANDLE g_cancelEvent = nullptr;
std::vector<HANDLE> g_workerHandles;
int g_workerCount = 0;
LONG g_nextQueueRegion = 0;
LONG g_nextPingRegion = 0;
LONG g_refreshGeneration = 0;

static const UINT ID_REFRESH = 1002, ID_APPLY = 1003, ID_LIST = 1001;
static const UINT_PTR TIMER_SPINNER = 1, TIMER_APPLY_FLASH = 2;
static const wchar_t* SPINNER_FRAMES[] = { L"\u25D0", L"\u25D3", L"\u25D1", L"\u25D2" }; // spinner frames

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
// HTTP GET via WinINet. No exceptions, no <string> heap churn beyond result.
// ---------------------------------------------------------------------------
static std::string HttpGet(HINTERNET hInternet, const wchar_t* url) {
    std::string result;
    result.reserve(256);
    if (!hInternet) return result;
    HINTERNET hUrl = InternetOpenUrlW(hInternet, url, nullptr, 0,
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
            char buf[1024]; DWORD bytesRead = 0;
            while (result.size() < MAX_HTTP_BODY &&
                   InternetReadFile(hUrl, buf,
                       (DWORD)((MAX_HTTP_BODY - result.size()) < sizeof(buf) ?
                           (MAX_HTTP_BODY - result.size()) : sizeof(buf)), &bytesRead) &&
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
    if (!length || length >= MAX_PATH) return ToWide(REGION_CACHE_NAME);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    return std::wstring(path) + ToWide(REGION_CACHE_NAME);
}

static void ResetRegionMetrics(Region& region) {
    wcscpy_s(region.killer, _countof(region.killer), L"-");
    wcscpy_s(region.survivor, _countof(region.survivor), L"-");
    wcscpy_s(region.ping, _countof(region.ping), L"-");
    region.killerSecs = INT_MAX;
    region.survivorSecs = INT_MAX;
    region.pingMs = INT_MAX;
}

static void BuildRegions(const std::vector<std::pair<std::string, bool>>& entries) {
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
}

static bool LoadRegionCache(std::vector<std::pair<std::string, bool>>& entries) {
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

static void SaveRegionCache(const std::vector<std::pair<std::string, bool>>& entries) {
    std::ofstream file(RegionCachePath(), std::ios::binary | std::ios::trunc);
    if (!file) return;
    for (const auto& entry : entries) file << entry.first << '\t' << (entry.second ? '1' : '0') << '\n';
}

static bool DiscoverRegions(std::vector<std::pair<std::string, bool>>& entries) {
    HINTERNET internet = InternetOpenW(L"DBDRegionChanger/2.0", INTERNET_OPEN_TYPE_PRECONFIG,
                                       nullptr, nullptr, 0);
    if (!internet) return false;
    DWORD timeoutMs = 6000;
    InternetSetOptionW(internet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionW(internet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    std::string body = HttpGet(internet, REGIONS_URL);
    InternetCloseHandle(internet);
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

void SortByColumn(int col, bool toggle);
void PopulateList(HWND hList);

void InitializeRegions() {
    std::vector<std::pair<std::string, bool>> entries;
    if (DiscoverRegions(entries)) {
        SaveRegionCache(entries);
    } else {
        entries.clear();
        LoadRegionCache(entries);
    }
    BuildRegions(entries);
}

void RefreshRegionStatus(HWND hList) {
    std::vector<std::pair<std::string, bool>> entries;
    if (!DiscoverRegions(entries)) return;

    std::vector<std::pair<std::string, bool>> previous;
    for (int i = 0; i < REGION_COUNT; ++i) {
        previous.emplace_back(g_regions[i].code, g_unblockedRegions[i]);
    }
    SaveRegionCache(entries);
    BuildRegions(entries);
    g_unblockedRegions.assign(REGION_COUNT, false);
    for (int i = 0; i < REGION_COUNT; ++i) {
        for (const auto& oldRegion : previous) {
            if (oldRegion.first == g_regions[i].code) {
                g_unblockedRegions[i] = oldRegion.second;
                break;
            }
        }
    }
    g_order.resize(REGION_COUNT);
    for (int i = 0; i < REGION_COUNT; ++i) g_order[i] = i;
    SortByColumn(g_sortColumn, false);
    PopulateList(hList);
}

static int ParseDurationSeconds(const wchar_t* s);

static bool ExtractQueueTime(const std::string& body, const char* regionCode,
                             const char* role, wchar_t* output, size_t outputLength) {
    std::string region = std::string("\"") + regionCode + "\"";
    size_t regionPos = body.find(region);
    if (regionPos == std::string::npos) return false;
    size_t rolePos = body.find(std::string("\"") + role + "\"", regionPos);
    if (rolePos == std::string::npos) return false;
    size_t timePos = body.find("\"time\"", rolePos);
    if (timePos == std::string::npos) return false;
    size_t colon = body.find(':', timePos);
    if (colon == std::string::npos) return false;
    size_t quote = body.find('"', colon + 1);
    size_t end = quote == std::string::npos ? std::string::npos : body.find('"', quote + 1);
    if (end == std::string::npos || end - quote - 1 >= outputLength) return false;
    std::string seconds = body.substr(quote + 1, end - quote - 1);
    for (unsigned char ch : seconds) if (ch < '0' || ch > '9') return false;
    std::wstring value = ToWide(seconds.c_str());
    unsigned long long total = _wcstoui64(value.c_str(), nullptr, 10);
    if (total > INT_MAX) return false;
    swprintf_s(output, outputLength, L"%llu:%02llu", total / 60, total % 60);
    return true;
}

static bool ParseQueueResponse(const std::string& body, QueueSnapshot* snapshots) {
    if (body.empty()) return false;
    bool parsedAny = false;
    for (int i = 0; i < REGION_COUNT; ++i) {
        if (!g_regions[i].online) continue;
        wchar_t value[24];
        if (ExtractQueueTime(body, g_regions[i].code.c_str(), "killer", value, _countof(value))) {
            parsedAny = true;
            wcscpy_s(snapshots[i].killer, _countof(snapshots[i].killer), value);
            snapshots[i].killerSecs = ParseDurationSeconds(value);
        }
        if (ExtractQueueTime(body, g_regions[i].code.c_str(), "survivor", value, _countof(value))) {
            parsedAny = true;
            wcscpy_s(snapshots[i].survivor, _countof(snapshots[i].survivor), value);
            snapshots[i].survivorSecs = ParseDurationSeconds(value);
        }
    }
    return parsedAny;
}

// "5m12s" / "3m" / "45s" / "N/A" -> total seconds. N/A -> INT_MAX (sorts last).
static int ParseDurationSeconds(const wchar_t* s) {
    if (!s || !*s || wcscmp(s, L"N/A") == 0) return INT_MAX;
    const wchar_t* separator = wcschr(s, L':');
    if (separator) {
        unsigned long long minutes = 0;
        unsigned long long seconds = 0;
        for (const wchar_t* p = s; p < separator; ++p) {
            if (*p < L'0' || *p > L'9') return INT_MAX;
            minutes = minutes * 10 + (unsigned long long)(*p - L'0');
            if (minutes > INT_MAX) return INT_MAX;
        }
        if (!separator[1] || separator[2] < L'0' || separator[2] > L'9' ||
            separator[1] < L'0' || separator[1] > L'9' || separator[3]) return INT_MAX;
        seconds = (unsigned long long)(separator[1] - L'0') * 10 + (separator[2] - L'0');
        if (minutes > (INT_MAX - seconds) / 60) return INT_MAX;
        return (int)(minutes * 60 + seconds);
    }
    unsigned long long minutes = 0;
    unsigned long long seconds = 0;
    const wchar_t* p = s;
    bool hasMinutes = false;
    bool hasSeconds = false;
    while (*p) {
        if (*p < L'0' || *p > L'9') return INT_MAX;
        unsigned long long value = 0;
        while (*p >= L'0' && *p <= L'9') {
            value = value * 10 + (unsigned long long)(*p - L'0');
            if (value > INT_MAX) return INT_MAX;
            ++p;
        }
        if (*p == L'm' && !hasMinutes && !hasSeconds) {
            minutes = value;
            hasMinutes = true;
        } else if (*p == L's' && !hasSeconds) {
            seconds = value;
            hasSeconds = true;
        } else {
            return INT_MAX;
        }
        ++p;
    }
    if ((!hasMinutes && !hasSeconds) || minutes > (INT_MAX - seconds) / 60) return INT_MAX;
    return (int)(minutes * 60 + seconds);
}

// toggle=true: clicking the same column flips direction (like Explorer);
// clicking a different column resets to ascending. toggle=false always
// forces ascending (used for the initial default sort).
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

static int TcpConnectPingAddress(const sockaddr* address, int addressLength, DWORD timeoutMs = 1200) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;

    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    connect(s, address, addressLength);

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
    int addressLength = (int)res->ai_addrlen;
    FreeAddrInfoW(res);

    int rtt = IcmpPingAddress(address);
    if (rtt >= 0) return rtt;
    sockaddr_in tcpAddress = {};
    tcpAddress.sin_family = AF_INET;
    tcpAddress.sin_addr = address;
    tcpAddress.sin_port = htons(443);
    return TcpConnectPingAddress((sockaddr*)&tcpAddress, addressLength);
}

void SortByColumn(int col, bool toggle) {
    if (toggle && col == g_sortColumn) g_sortAscending = !g_sortAscending;
    else g_sortAscending = true;
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
        if (aValue == INT_MAX || bValue == INT_MAX) {
            if (aValue != bValue) return aValue != INT_MAX;
        } else if (aValue != bValue) {
            return asc ? aValue < bValue : aValue > bValue;
        }
        return wcscmp(g_regions[a].label.c_str(), g_regions[b].label.c_str()) < 0;
    });
}
static const int QUEUE_WORKER_COUNT = 1;

struct FetchArgs { HWND hwnd; bool pingWorker; LONG generation; };

void UpdateRegionRow(HWND hList, int regionIdx);


static void PostFetchResult(HWND hwnd, FetchResult* result, bool completed) {
    if (completed) {
        InterlockedDecrement(&g_pendingFetches);
        InterlockedIncrement(&g_completedFetches);
    }
    if (!PostMessage(hwnd, WM_APP_REGION_DONE, 0, (LPARAM)result)) {
        delete result;
    }
}

static bool RefreshCancelled() {
    return g_cancelEvent && WaitForSingleObject(g_cancelEvent, 0) == WAIT_OBJECT_0;
}

void CloseWorkerHandles() {
    for (int i = 0; i < g_workerCount; ++i) {
        if (g_workerHandles[i]) {
            CloseHandle(g_workerHandles[i]);
            g_workerHandles[i] = nullptr;
        }
    }
    g_workerCount = 0;
}

static DWORD WINAPI FetchThreadProc(LPVOID param) {
    FetchArgs* args = (FetchArgs*)param;
    HWND hwnd = args->hwnd;
    bool pingWorker = args->pingWorker;
    LONG generation = args->generation;
    delete args;

    HINTERNET hInternet = pingWorker ? nullptr : InternetOpenW(L"DBDRegionChanger/2.0",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    DWORD timeoutMs = 6000;
    if (hInternet) {
        DWORD maxConnections = REGION_COUNT;
        InternetSetOptionW(hInternet, INTERNET_OPTION_MAX_CONNS_PER_SERVER,
                           &maxConnections, sizeof(maxConnections));
        InternetSetOptionW(hInternet, INTERNET_OPTION_MAX_CONNS_PER_1_0_SERVER,
                           &maxConnections, sizeof(maxConnections));
        InternetSetOptionW(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT,
                           &timeoutMs, sizeof(timeoutMs));
        InternetSetOptionW(hInternet, INTERNET_OPTION_SEND_TIMEOUT,
                           &timeoutMs, sizeof(timeoutMs));
        InternetSetOptionW(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT,
                           &timeoutMs, sizeof(timeoutMs));
    }

    LONG& nextRegion = pingWorker ? g_nextPingRegion : g_nextQueueRegion;
    while (true) {
        LONG idxValue = InterlockedIncrement(&nextRegion) - 1;
        if (idxValue >= REGION_COUNT) break;
        int idx = (int)idxValue;
        if (RefreshCancelled()) break;
        FetchResult* result = new FetchResult{};
        result->idx = idx;
        result->generation = generation;

        if (pingWorker) {
            result->pingStage = true;
            if (!g_regions[idx].online) {
                delete result;
                continue;
            }
            int rtt = MeasurePing(g_regions[idx].code.c_str());
            if (rtt >= 0) {
                swprintf_s(result->ping, 16, L"%dms", rtt);
                result->pingMs = rtt;
            }
            PostFetchResult(hwnd, result, true);
        } else {
            std::wstring queueUrl = std::wstring(QUEUES_URL) + L"?_=" +
                std::to_wstring(GetTickCount64());
            std::string body = HttpGet(hInternet, queueUrl.c_str());
            QueueFetchBatch* batch = new QueueFetchBatch{ generation,
                std::vector<QueueSnapshot>(REGION_COUNT) };
            if (!ParseQueueResponse(body, batch->snapshots.data())) {
                delete batch;
                batch = nullptr;
            }
            delete result;
            if (batch && !PostMessage(hwnd, WM_APP_QUEUE_DONE, 0, (LPARAM)batch)) {
                delete batch;
            }
            break;
        }
    }

    if (hInternet) InternetCloseHandle(hInternet);
    InterlockedDecrement(&g_activeWorkers);
    PostMessage(hwnd, WM_APP_WORKER_DONE, 0, 0);
    return 0;
}

void StartRefresh(HWND hwnd) {
    if (g_refreshing) return;
    if (REGION_COUNT <= 0) return;
    LONG generation = InterlockedIncrement(&g_refreshGeneration);

    for (int idx = 0; idx < REGION_COUNT; ++idx) ResetRegionMetrics(g_regions[idx]);
    PopulateList(g_hList);
    RedrawWindow(g_hList, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);

    RefreshRegionStatus(g_hList);
    if (!g_cancelEvent) g_cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_cancelEvent) ResetEvent(g_cancelEvent);
    if (!g_wsaStarted) {
        WSADATA wsaData;
        g_wsaStarted = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    }
    DWORD maxConnections = REGION_COUNT;
    InternetSetOptionW(nullptr, INTERNET_OPTION_MAX_CONNS_PER_SERVER,
                       &maxConnections, sizeof(maxConnections));
    InternetSetOptionW(nullptr, INTERNET_OPTION_MAX_CONNS_PER_1_0_SERVER,
                       &maxConnections, sizeof(maxConnections));
    g_refreshing = true;
    g_spinnerFrame = 0;
    EnableWindow(g_hBtnRefresh, FALSE);
    SetTimer(hwnd, TIMER_SPINNER, 120, nullptr);
    InvalidateRect(g_hBtnRefresh, nullptr, TRUE);

    LONG onlineCount = 0;
    for (const Region& region : g_regions) if (region.online) ++onlineCount;
    InterlockedExchange(&g_pendingFetches, onlineCount);
    InterlockedExchange(&g_completedFetches, 0);
    for (int idx = 0; idx < REGION_COUNT; ++idx) ResetRegionMetrics(g_regions[idx]);
    PopulateList(g_hList);
    RedrawWindow(g_hList, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
    g_workerCount = QUEUE_WORKER_COUNT + REGION_COUNT;
    g_workerHandles.assign(g_workerCount, nullptr);
    InterlockedExchange(&g_nextQueueRegion, 0);
    InterlockedExchange(&g_nextPingRegion, 0);
    InterlockedExchange(&g_activeWorkers, 0);
    for (int workerIndex = 0; workerIndex < g_workerCount; ++workerIndex) {
        FetchArgs* args = new FetchArgs{ hwnd, workerIndex >= QUEUE_WORKER_COUNT, generation };
        InterlockedIncrement(&g_activeWorkers);
        HANDLE h = CreateThread(nullptr, 128 * 1024, FetchThreadProc, args,
            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (h) {
            g_workerHandles[workerIndex] = h;
        } else {
            InterlockedDecrement(&g_activeWorkers);
            delete args;
            LONG& nextRegion = workerIndex >= QUEUE_WORKER_COUNT ? g_nextPingRegion : g_nextQueueRegion;
            while (true) {
                LONG idxValue = InterlockedIncrement(&nextRegion) - 1;
                if (idxValue >= REGION_COUNT) break;
                int idx = (int)idxValue;
                FetchResult* result = new FetchResult{};
                result->idx = idx;
                result->generation = generation;
                if (workerIndex >= QUEUE_WORKER_COUNT) {
                    if (!g_regions[idx].online) {
                        delete result;
                        continue;
                    }
                    result->pingStage = true;
                    PostFetchResult(hwnd, result, true);
                } else {
                    delete result;
                    QueueFetchBatch* batch = new QueueFetchBatch{ generation,
                        std::vector<QueueSnapshot>(REGION_COUNT) };
                    if (!PostMessage(hwnd, WM_APP_QUEUE_DONE, 0, (LPARAM)batch)) {
                        delete batch;
                    }
                    break;
                }
            }
        }
    }
}
