#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>   // for OpenProcessToken, GetTokenInformation
// fstream already provides wofstream
#include <string>
#include <vector>
#include <locale>
#include <fstream>
// Convert a wide string to narrow using Windows API to avoid deprecated codecvt
static std::string WstringToString(const std::wstring& ws) {
    if (ws.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &result[0], size_needed, nullptr, nullptr);
    return result;
}
#include "hosts_file.h"
#include "region_service.h"

// ---------------------------------------------------------------------------
//  Helper to get the full path to the hosts file in a Unicode‑safe way.
// ---------------------------------------------------------------------------
static std::wstring GetHostsPath() {
    wchar_t sysPath[MAX_PATH] = {};
    DWORD len = GetSystemDirectoryW(sysPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L"C:\\Windows\\System32";
    std::wstring path = sysPath;
    path += L"\\drivers\\etc\\hosts";
    return path;
}

static std::wstring GetHostsDirPath() {
    wchar_t sysPath[MAX_PATH] = {};
    DWORD len = GetSystemDirectoryW(sysPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L"C:\\Windows\\System32";
    std::wstring path = sysPath;
    path += L"\\drivers\\etc";
    return path;
}

// ---------------------------------------------------------------------------
//  Check if the current process has administrative rights.
// ---------------------------------------------------------------------------
static bool IsRunningAsAdmin() {
    BOOL isElevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation = {};
        DWORD retLen = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &retLen)) {
            isElevated = elevation.TokenIsElevated;
        }
        CloseHandle(token);
    }
    return isElevated != 0;
}
static const char* HOST_PREFIX = "gamelift-ping.";
static const char* HOST_SUFFIX = ".api.aws";
static const char* BLOCK_START = "# --- DBDRegionChanger managed block ---";
static const char* BLOCK_END   = "# --- end DBDRegionChanger managed block ---";

static bool HasHostToken(const char* text, const std::string& host) {
    const char* match = strstr(text, host.c_str());
    while (match) {
        const char* end = match + host.size();
        bool beforeIsBoundary = match == text || match[-1] == ' ' || match[-1] == '\t';
        bool afterIsBoundary = *end == '\0' || *end == ' ' || *end == '\t' ||
            *end == '\r' || *end == '\n' || *end == '#';
        if (beforeIsBoundary && afterIsBoundary) return true;
        match = strstr(end, host.c_str());
    }
    return false;
}

static bool IsGameliftLine(const char* line, std::string& bareOut) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#') {
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
    }
    bareOut = p;
    for (int i = 0; i < REGION_COUNT; ++i) {
        std::string host = std::string(HOST_PREFIX) + g_regions[i].code + HOST_SUFFIX;
        if (HasHostToken(bareOut.c_str(), host)) return true;
    }
    return false;
}

// Strips \r\n so marker lines can be compared exactly regardless of line ending.
static std::string TrimEol(const char* line) {
    std::string t = line;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    return t;
}

static bool FlushFileToDisk(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool flushed = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return flushed;
}

static bool OverwriteFileFromTemp(const wchar_t* tempPath, const wchar_t* targetPath) {
    HANDLE source = CreateFileW(tempPath, GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE) return false;
    HANDLE target = CreateFileW(targetPath, GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (target == INVALID_HANDLE_VALUE) {
        CloseHandle(source);
        return false;
    }

    LARGE_INTEGER zero = {};
    bool ok = SetFilePointerEx(target, zero, nullptr, FILE_BEGIN) && SetEndOfFile(target);
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ok) {
        if (!ReadFile(source, buffer, sizeof(buffer), &bytesRead, nullptr)) {
            ok = false;
            break;
        }
        if (bytesRead == 0) break;
        DWORD written = 0;
        ok = WriteFile(target, buffer, bytesRead, &written, nullptr) && written == bytesRead;
    }
    ok = ok && FlushFileBuffers(target);
    CloseHandle(target);
    CloseHandle(source);
    return ok;
}

static void FlushDnsCache() {
    wchar_t commandLine[] = L"ipconfig.exe /flushdns";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, commandLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return;
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

bool ApplyRegions(const std::vector<bool>& unblocked, std::wstring& errOut) {
    if (!IsRunningAsAdmin()) {
        errOut = L"Administrative privileges required to modify the hosts file.";
        return false;
    }
    if ((int)unblocked.size() != REGION_COUNT) {
        errOut = L"Invalid region selection.";
        return false;
    }
    bool hasUnblockedRegion = false;
    for (bool enabled : unblocked) if (enabled) {
        hasUnblockedRegion = true;
        break;
    }
    if (!hasUnblockedRegion) {
        errOut = L"Select at least one region before applying.";
        return false;
    }
    std::ifstream in(WstringToString(GetHostsPath()), std::ios::binary);
    if (!in) {
        errOut = L"Could not open hosts file. Run as Administrator.";
        return false;
    }
    std::vector<std::string> kept;
    {
        std::string line;
        bool inBlock = false;
        while (std::getline(in, line)) {
            std::string lineWithEol = line + "\n";
            std::string trimmed = TrimEol(lineWithEol.c_str());
            if (!inBlock && trimmed == BLOCK_START) { inBlock = true; continue; }
            if (inBlock) {
                if (trimmed == BLOCK_END) inBlock = false;
                continue;
            }
            std::string bare;
            if (IsGameliftLine(lineWithEol.c_str(), bare)) continue;
            kept.push_back(std::move(lineWithEol));
        }
        if (!in.eof()) {
            errOut = L"Could not read hosts file. Run as Administrator.";
            return false;
        }
    }

    // Drop trailing blank lines so we control spacing before our block
    // instead of accumulating blank lines from previous runs.
    while (!kept.empty() && TrimEol(kept.back().c_str()).empty()) kept.pop_back();

    std::wstring backupPath = GetHostsPath() + L".dbdregionchanger.bak";
    wchar_t tempPath[MAX_PATH] = {};
    if (GetTempFileNameW(GetHostsDirPath().c_str(), L"DBD", 0, tempPath) == 0) {
        errOut = L"Could not create temporary hosts file. Run as Administrator.";
        return false;
    }
    std::wofstream out(tempPath, std::ios::trunc);
    if (!out) {
        DeleteFileW(tempPath);
        errOut = L"Could not write hosts file. Run as Administrator.";
        return false;
    }
    bool writeOk = true;
    for (const auto& l : kept) out << std::wstring(l.begin(), l.end());
    out << L"\n";
    out << L"# --- DBDRegionChanger managed block ---\n";
    for (int i = 0; i < REGION_COUNT; ++i) {
        std::wstring host = std::wstring(L"gamelift-ping.") + std::wstring(g_regions[i].code.begin(), g_regions[i].code.end()) + std::wstring(L".api.aws");
        out << (unblocked[i] ? L"# 0.0.0.0 " : L"0.0.0.0 ") << host << L"\n";
    }
    out << L"# --- end DBDRegionChanger managed block ---\n";
    out.flush();
    writeOk = out.good();
    out.close();
    bool closed = !out.fail();
    bool flushed = writeOk && closed && FlushFileToDisk(tempPath);
    DeleteFileW(backupPath.c_str());
    bool backedUp = flushed && CopyFileW(GetHostsPath().c_str(), backupPath.c_str(), FALSE) != FALSE;
    bool replaced = backedUp && OverwriteFileFromTemp(tempPath, GetHostsPath().c_str());
    DeleteFileW(tempPath);
    if (!replaced) {
        DWORD error = GetLastError();
        wchar_t message[128];
        swprintf_s(message, L"Could not update hosts file (error %lu). Run as Administrator.", error);
        errOut = message;
        return false;
    }

    FlushDnsCache();
    return true;
}

void DetectUnblockedRegions() {
    if (!IsRunningAsAdmin()) {
        // If not admin, we cannot read the hosts file reliably – skip.
        return;
    }
    g_unblockedRegions.assign(REGION_COUNT, false);
    std::ifstream in(WstringToString(GetHostsPath()), std::ios::binary);
    if (!in) return;
    std::string line;
    bool inBlock = false;
    bool foundBlock = false;
    while (std::getline(in, line)) {
        const char* p = line.c_str();
        // Trim leading whitespace for comparison
        while (*p == ' ' || *p == '\t') ++p;

        if (!inBlock) {
            // Check if we're entering the managed block
            if (strncmp(p, BLOCK_START, strlen(BLOCK_START)) == 0) {
                inBlock = true;
                foundBlock = true;
            }
            continue;
        }

        // We are inside the managed block – check for end marker
        if (strncmp(p, BLOCK_END, strlen(BLOCK_END)) == 0) {
            inBlock = false;
            continue;
        }

        // Inside block: look for # 0.0.0.0 gamelift-ping.* entries
        const char* q = line.c_str();
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != '#') continue;
        ++q;
        while (*q == ' ' || *q == '\t') ++q;
        if (strncmp(q, "0.0.0.0", 7) != 0 ||
            (q[7] != ' ' && q[7] != '\t')) continue;
        q += 7;
        while (*q == ' ' || *q == '\t') ++q;
        for (int i = 0; i < REGION_COUNT; ++i) {
            std::string host = std::string(HOST_PREFIX) + g_regions[i].code + HOST_SUFFIX;
            if (HasHostToken(q, host)) g_unblockedRegions[i] = true;
        }
    }
    // If no managed block was found at all, leave g_unblockedRegions as-is
    // (all false = nothing to restore, user hasn't applied changes yet)
}
