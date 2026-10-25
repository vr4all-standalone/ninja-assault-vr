@echo off
rem Build this Flycast fork for Windows x64 with MSVC + Ninja (same recipe as Flycast's CI),
rem for PCVR (OpenXR: SteamVR, Quest Link, Virtual Desktop), the finger-gun mode and testing.
rem Usage: build-win.cmd [Release|RelWithDebInfo|Debug] [ON|OFF]
rem Standalone Ninja Assault defaults ON for the PC package; pass OFF for inherited-game regressions.
rem Needs Visual Studio 2022 (or its Build Tools) with the C++ workload, found with vswhere.
rem The CMake and Ninja that come with it are called by full path, so another CMake earlier
rem in PATH (devkitPro's msys2 one, say) can't get in the way: it cannot drive MSVC.
setlocal
set BUILD_TYPE=%1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=RelWithDebInfo
set STANDALONE_NINJA=%~2
if "%STANDALONE_NINJA%"=="" set STANDALONE_NINJA=ON
if /I not "%STANDALONE_NINJA%"=="ON" if /I not "%STANDALONE_NINJA%"=="OFF" (
    echo Standalone policy must be ON or OFF.
    exit /b 1
)
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
	echo Visual Studio with the C++ tools was not found.
	exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
cd /d "%~dp0.." || exit /b 1
"%CMAKE%" -S . -B build-win -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DUSE_DX9=OFF -DUSE_OPENXR=ON -DVR_STANDALONE_NINJA=%STANDALONE_NINJA% -DVR_STANDALONE_LUPIN=OFF || exit /b 1
"%CMAKE%" --build build-win --parallel || exit /b 1
echo.
echo Built: %CD%\build-win\flycast.exe
