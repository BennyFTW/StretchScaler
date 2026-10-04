@echo off
rem StretchScaler build script - MSVC (VS 2022 / Build Tools), no CMake required.
rem Works when double-clicked or run from any directory.
setlocal EnableExtensions
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found: "%VSWHERE%"
    echo         Install Visual Studio 2022 or Build Tools 2022 with "Desktop development with C++".
    set "RC=1"
    goto :done
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo [ERROR] No Visual Studio installation with the MSVC x64 toolset was found.
    echo         Install the "Desktop development with C++" workload and a Windows 10/11 SDK.
    set "RC=1"
    goto :done
)

set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat not found: "%VCVARS%"
    set "RC=1"
    goto :done
)

echo Using: %VSINSTALL%
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed.
    set "RC=1"
    goto :done
)
rem vcvars may change the current directory - go back to the project root.
cd /d "%~dp0"

if not exist obj mkdir obj
if not exist bin mkdir bin

echo Building bin\StretchScaler.exe ...
rc.exe /nologo /fo obj\app.res src\app.rc
if errorlevel 1 (
    echo [ERROR] Resource compile failed.
    set "RC=1"
    goto :done
)
cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /permissive- /Zc:__cplusplus /utf-8 /Fo"obj\\" /Fe"bin\StretchScaler.exe" src\*.cpp obj\app.res /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:src\app.manifest d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib windowsapp.lib user32.lib gdi32.lib comctl32.lib Magnification.lib ole32.lib
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo [SUCCESS] Built "%~dp0bin\StretchScaler.exe"
) else (
    echo [FAILED] Build failed with error %RC%.
)

:done
rem Keep the window open when started by double-click from Explorer.
echo %CMDCMDLINE% | findstr /i /c:"%~nx0" >nul && pause
endlocal & exit /b %RC%
