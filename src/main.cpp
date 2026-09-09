#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <dwmapi.h>

#include "region_service.h"
#include "ui.h"

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    InitializeRegions();

    const wchar_t* className = L"DBDRegionChangerWnd2";
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = hInst;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    RegisterClassExW(&windowClass);

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int windowWidth = 480;
    const int windowHeight = 450;
    int windowX = workArea.left + ((workArea.right - workArea.left) - windowWidth) / 2;
    int windowY = workArea.top + ((workArea.bottom - workArea.top) - windowHeight) / 2;
    HWND window = CreateWindowExW(0, className, L"DBD Region Changer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_MAXIMIZEBOX,
        windowX, windowY, windowWidth, windowHeight, nullptr, nullptr, hInst, nullptr);
    if (window) {
        BOOL useDarkTitleBar = TRUE;
        DwmSetWindowAttribute(window, 20, &useDarkTitleBar, sizeof(useDarkTitleBar));
    }

    ShowWindow(window, nCmdShow);
    UpdateWindow(window);
    MSG message;
    while (GetMessage(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return 0;
}
