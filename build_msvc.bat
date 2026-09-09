@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

REM Automatically initialize the x64 MSVC environment when this script is
REM launched from a normal PowerShell, Command Prompt, or VS Code terminal.
where cl >nul 2>&1
if errorlevel 1 (
   set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
   if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
   if not exist "!VSWHERE!" (
      echo Visual Studio Installer was not found.
      echo Install Visual Studio Build Tools with the C++ desktop workload.
      goto :err
   )

   for /f "usebackq delims=" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
   if not defined VSINSTALL (
      echo The MSVC C++ workload was not found.
      echo Install Visual Studio Build Tools with the C++ desktop workload.
      goto :err
   )
   call "!VSINSTALL!\Common7\Tools\VsDevCmd.bat" -arch=x64
   if errorlevel 1 goto :err
)

where cl >nul 2>&1
if errorlevel 1 (
   echo MSVC compiler was not found.
   goto :err
)
where rc >nul 2>&1
if errorlevel 1 (
   echo Windows Resource Compiler was not found.
   goto :err
)

REM Compile resources (app.rc references app.manifest in the same directory)
cd /d "%~dp0resources"
rc app.rc
if errorlevel 1 (cd /d "%~dp0" & goto :err)
cd /d "%~dp0"

REM /Os = optimize for size, /GL + /LTCG = whole-program optimization.
REM /OPT:REF,ICF = strip unused code and fold duplicate functions.
REM /GS, /sdl, /guard:cf, ASLR, and NXCOMPAT enable release hardening.

cl -Iinclude /O1 /Os /GL /GS /sdl /guard:cf /EHsc /std:c++17 /DNDEBUG ^
    src\main.cpp src\ui.cpp src\region_service.cpp src\hosts_file.cpp ^
    resources\app.res ^
    user32.lib gdi32.lib comctl32.lib wininet.lib uxtheme.lib dwmapi.lib ws2_32.lib iphlpapi.lib advapi32.lib ^
    delayimp.lib /link /SUBSYSTEM:WINDOWS /LTCG /guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
    /DELAYLOAD:wininet.dll /DELAYLOAD:ws2_32.dll /DELAYLOAD:iphlpapi.dll /OUT:DBDRegionChanger.exe
if errorlevel 1 goto :err

echo.
echo Build OK: DBDRegionChanger.exe
for %%A in (DBDRegionChanger.exe) do echo Size: %%~zA bytes

REM Clean up intermediate files
del /q *.obj resources\*.res 2>nul
goto :eof

:err
echo Build failed.
exit /b 1
