#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <windows.h>
#include <dwmapi.h>
#include <algorithm>

#include "region_service.h"
#include "ui.h"

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    // The process runs elevated, so never load DLLs from the app directory
    // (typically Downloads). Covers the delay-loaded network DLLs.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    WSADATA wsaData;
    bool wsaStarted = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    InitializeRegions();

    const wchar_t* className = L"DBDRegionChangerWnd2";
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = hInst;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    windowClass.hbrBackground = nullptr;
    RegisterClassExW(&windowClass);

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    // The window opens on the primary monitor, whose DPI is the system DPI.
    const int dpi = (int)GetDpiForSystem();
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME |
                        WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    // Size the window so the list ends right below its last row.
    RECT frame = { 0, 0, 0, ClientHeightForRows(REGION_COUNT, (UINT)dpi) };
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, (UINT)dpi);
    const int windowWidth = MulDiv(580, dpi, USER_DEFAULT_SCREEN_DPI);
    const int windowHeight = (std::min)((int)(frame.bottom - frame.top),
                                        (int)(workArea.bottom - workArea.top));
    int windowX = workArea.left + ((workArea.right - workArea.left) - windowWidth) / 2;
    int windowY = workArea.top + ((workArea.bottom - workArea.top) - windowHeight) / 2;
    HWND window = CreateWindowExW(0, className, L"DBD Region Changer", style,
        windowX, windowY, windowWidth, windowHeight, nullptr, nullptr, hInst, nullptr);
    if (window) {
        BOOL useDarkTitleBar = TRUE;
        DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &useDarkTitleBar, sizeof(useDarkTitleBar));
    }

    ShowWindow(window, nCmdShow);
    UpdateWindow(window);
    MSG message;
    while (GetMessage(&message, nullptr, 0, 0)) {
        if (IsDialogMessageW(window, &message)) continue; // Tab / arrow-key navigation
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    if (wsaStarted) WSACleanup();
    return 0;
}
