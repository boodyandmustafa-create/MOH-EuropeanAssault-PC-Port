@echo off
rem Builds Launcher.exe and puts it next to MOHEuropeanAssault.exe (one folder up).
cd /d "%~dp0"
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "CMAKE="
if exist "%VSW%" (
    for /f "usebackq delims=" %%i in (`"%VSW%" -latest -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`) do set "CMAKE=%%i"
)
if not defined CMAKE (
    echo Visual Studio with the C++ workload was not found.
    exit /b 1
)
"%CMAKE%" -S . -B build -A x64
if errorlevel 1 exit /b 1
"%CMAKE%" --build build --config Release
if errorlevel 1 exit /b 1
copy /Y build\Release\Launcher.exe ..\Launcher.exe >nul
echo Launcher.exe is ready.
