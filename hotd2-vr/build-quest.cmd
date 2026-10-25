@echo off
rem Build Ninja Assault VR for Quest: standalone arm64, optimised "vr" variant.
rem Needs a JDK, Android SDK, NDK 29.0.14206865 and CMake 3.22.1.
rem Explicit JAVA_HOME / ANDROID_HOME / GRADLE_USER_HOME always take priority.
setlocal
if not defined JAVA_HOME for /d %%J in ("%ProgramFiles%\Microsoft\jdk-17*") do if exist "%%J\bin\java.exe" set "JAVA_HOME=%%J"
if not defined JAVA_HOME if exist "E:\UnityEd\6000.0.78f1\Editor\Data\PlaybackEngines\AndroidPlayer\OpenJDK\bin\java.exe" set "JAVA_HOME=E:\UnityEd\6000.0.78f1\Editor\Data\PlaybackEngines\AndroidPlayer\OpenJDK"
if not defined JAVA_HOME for /d %%J in ("E:\_androidtools\tmp\jdk\jdk-*") do if exist "%%J\bin\java.exe" set "JAVA_HOME=%%J"
if not defined JAVA_HOME (
  echo Set JAVA_HOME to a JDK before building Ninja Assault VR.
  exit /b 1
)
if not defined ANDROID_HOME if exist "E:\AndroidSdk\build-tools" set "ANDROID_HOME=E:\AndroidSdk"
if not defined ANDROID_HOME set "ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk"
if not defined GRADLE_USER_HOME if exist "E:\.gradle\wrapper" set "GRADLE_USER_HOME=E:\.gradle"
cd /d "%~dp0..\shell\android-studio" || exit /b 1
rem The Gradle daemon toolchain also needs JDK 21 (installed or managed by Gradle).
call "%CD%\gradlew.bat" assembleVr --console=plain %* || exit /b 1
echo.
echo Built: %~dp0..\shell\android-studio\flycast\build\outputs\apk\vr\flycast-vr.apk
