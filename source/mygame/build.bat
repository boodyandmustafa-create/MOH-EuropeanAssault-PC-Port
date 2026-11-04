@echo off
rem Builds the game and puts MOHEuropeanAssault.exe next to the launchers (play_*.bat).
rem Needs Visual Studio 2022 or newer with "Desktop development with C++" (it brings its own CMake).
cd /d "%~dp0"
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "CMAKE="
if exist "%VSW%" (
    for /f "usebackq delims=" %%i in (`"%VSW%" -latest -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`) do set "CMAKE=%%i"
)
if not defined CMAKE (
    echo Visual Studio with the C++ workload was not found. Install it from https://visualstudio.microsoft.com/
    pause
    exit /b 1
)
"%CMAKE%" -S . -B build -A x64
if errorlevel 1 goto fail
"%CMAKE%" --build build --config Release --target your_game_recomp
if errorlevel 1 goto fail
copy /Y build\Release\your_game_recomp.exe MOHEuropeanAssault.exe >nul
if exist build\Release\your_game_recomp.pdb copy /Y build\Release\your_game_recomp.pdb MOHEuropeanAssault.pdb >nul
echo.
echo Done: MOHEuropeanAssault.exe is ready. Start the game with play_120fps_mkb.bat (keyboard + mouse) or play_120fps.bat.
pause
exit /b 0
:fail
echo Build failed.
pause
exit /b 1
