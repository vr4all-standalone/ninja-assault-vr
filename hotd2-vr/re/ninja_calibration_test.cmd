@echo off
rem Ninja Assault direct JYU coordinate calibration regression tests.
setlocal
set "ROOT=%~dp0..\.."
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%ROOT%\build-win\vrtest\ninja-calibration-unit"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%" || exit /b 1
pushd "%OUT%" || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /W3 /I"%ROOT%\core\rend\vr" /Feninja_calibration_test.exe "%~dp0ninja_calibration_test.cpp" >build.log 2>&1
if errorlevel 1 (
 type build.log
 popd
 exit /b 1
)
"%OUT%\ninja_calibration_test.exe"
set "CODE=%ERRORLEVEL%"
popd
exit /b %CODE%
