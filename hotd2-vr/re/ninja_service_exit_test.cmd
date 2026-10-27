@echo off
rem Ninja native Service exit regression tests.
rem Usage: ninja_service_exit_test.cmd [output directory]. Requires Visual Studio 2022 C++.
setlocal
set ROOT=%~dp0..\..
set OUT=%~1
if "%OUT%"=="" set OUT=%ROOT%\build-win\vrtest\ninja-service-exit-unit
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
	echo Visual Studio with the C++ tools was not found.
	exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%" || exit /b 1
pushd "%OUT%" || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /W3 /I"%ROOT%\core\rend\vr" /Feninja_service_exit_test.exe "%~dp0ninja_service_exit_test.cpp" >build.log 2>&1
if errorlevel 1 (
	type build.log
	echo ninja_service_exit_test: the build failed
	popd
	exit /b 1
)
"%OUT%\ninja_service_exit_test.exe"
set CODE=%ERRORLEVEL%
popd
exit /b %CODE%
