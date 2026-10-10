#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include "hosts_file.h"
#include "region_service.h"

// ---------------------------------------------------------------------------
//  Full path of a file under the System32 directory, e.g. L"\\drivers\\etc".
// ---------------------------------------------------------------------------
static std::wstring SystemPath(const wchar_t* relative) {
    wchar_t sysPath[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(sysPath, MAX_PATH);
    std::wstring path = (len == 0 || len >= MAX_PATH) ? L"C:\\Windows\\System32" : sysPath;
    return path + relative;
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

static bool IsGameliftLine(const char* line) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#') {
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
    }
    for (int i = 0; i < REGION_COUNT; ++i) {
        std::string host = std::string(HOST_PREFIX) + g_regions[i].code + HOST_SUFFIX;
        if (HasHostToken(p, host)) return true;
    }
    return false;
}

// Strips trailing \r so marker lines compare exactly and repeated CRs from
// earlier corrupted writes are repaired.
static void TrimCr(std::string& line) {
    while (!line.empty() && line.back() == '\r') line.pop_back();
}

static bool WriteFileBytes(const wchar_t* path, const std::string& data) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr) &&
              written == data.size() && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
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
    // Absolute path: a bare "ipconfig.exe" is searched for in the app directory
    // first, which would let a planted executable run elevated.
    std::wstring application = SystemPath(L"\\ipconfig.exe");
    wchar_t commandLine[] = L"ipconfig.exe /flushdns";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(application.c_str(), commandLine, nullptr, nullptr, FALSE,
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
    std::wstring hostsPath = SystemPath(L"\\drivers\\etc\\hosts");
    std::ifstream in(hostsPath.c_str(), std::ios::binary);
    if (!in) {
        errOut = L"Could not open hosts file. Run as Administrator.";
        return false;
    }
    std::vector<std::string> kept;
    {
        std::string line;
        bool inBlock = false;
        while (std::getline(in, line)) {
            TrimCr(line);
            if (!inBlock && line == BLOCK_START) { inBlock = true; continue; }
            if (inBlock) {
                if (line == BLOCK_END) inBlock = false;
                continue;
            }
            if (IsGameliftLine(line.c_str())) continue;
            kept.push_back(std::move(line));
        }
        if (!in.eof()) {
            errOut = L"Could not read hosts file. Run as Administrator.";
            return false;
        }
        in.close(); // the stream does not share delete access, which ReplaceFileW needs
    }

    // Drop trailing blank lines so we control spacing before our block
    // instead of accumulating blank lines from previous runs.
    while (!kept.empty() && kept.back().empty()) kept.pop_back();

    // Lines are kept as raw bytes (the hosts file may contain UTF-8 or ANSI
    // comments) and written back with CRLF endings.
    std::string output;
    for (const auto& l : kept) output += l + "\r\n";
    output += "\r\n";
    output += std::string(BLOCK_START) + "\r\n";
    for (int i = 0; i < REGION_COUNT; ++i) {
        output += unblocked[i] ? "# 0.0.0.0 " : "0.0.0.0 ";
        output += HOST_PREFIX + g_regions[i].code + HOST_SUFFIX + "\r\n";
    }
    output += std::string(BLOCK_END) + "\r\n";

    std::wstring backupPath = hostsPath + L".dbdregionchanger.bak";
    wchar_t tempPath[MAX_PATH] = {};
    if (GetTempFileNameW(SystemPath(L"\\drivers\\etc").c_str(), L"DBD", 0, tempPath) == 0) {
        errOut = L"Could not create temporary hosts file. Run as Administrator.";
        return false;
    }
    // ReplaceFileW swaps the files atomically, keeps the hosts file's ACL and
    // attributes, and writes the backup in the same step. If it fails (e.g.
    // another process holds the hosts file open), copy the contents in place.
    bool replaced = false;
    if (WriteFileBytes(tempPath, output)) {
        replaced = ReplaceFileW(hostsPath.c_str(), tempPath, backupPath.c_str(),
                                REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) ||
                   (CopyFileW(hostsPath.c_str(), backupPath.c_str(), FALSE) &&
                    OverwriteFileFromTemp(tempPath, hostsPath.c_str()));
    }
    DWORD error = GetLastError();
    DeleteFileW(tempPath);
    if (!replaced) {
        wchar_t message[128];
        swprintf_s(message, L"Could not update hosts file (error %lu). Run as Administrator.", error);
        errOut = message;
        return false;
    }

    FlushDnsCache();
    return true;
}

void DetectUnblockedRegions() {
    // Sized before anything can fail: the UI indexes this per region.
    // Reading the hosts file does not require elevation.
    g_unblockedRegions.assign(REGION_COUNT, false);
    std::ifstream in(SystemPath(L"\\drivers\\etc\\hosts").c_str(), std::ios::binary);
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
    // Without a managed block nothing is blocked, so every region is allowed.
    if (!foundBlock) g_unblockedRegions.assign(REGION_COUNT, true);
}
