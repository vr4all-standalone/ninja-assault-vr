@echo off
rem Tests loaded-game selection with standalone policy ON and normal Flycast policy OFF.
rem Usage: standalone_game_test.cmd [output directory]
setlocal
set ROOT=%~dp0..\..
set OUT=%~1
if "%OUT%"=="" set OUT=%ROOT%\build-win\vrtest\ninja-panel-policy-unit
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo Visual Studio with the C++ tools was not found.
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%" || exit /b 1
pushd "%OUT%" || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /W3 /I"%ROOT%\core" /DVR_STANDALONE_NINJA=1 /Feninja-panel.exe "%~dp0ninja_panel_test.cpp" >build-ninja.log 2>&1
if errorlevel 1 (type build-ninja.log & popd & exit /b 1)
ninja-panel.exe
set RESULT=%ERRORLEVEL%
popd
exit /b %RESULT%