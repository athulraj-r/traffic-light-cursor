@echo off
setlocal

echo === Traffic Light Cursor - Build Script ===

REM Use the MSYS2/UCRT64 MinGW toolchain found on this machine
set CMAKE="C:\Program Files\CMake\bin\cmake.exe"
set GENERATOR="MinGW Makefiles"
set MAKE="C:\msys64\ucrt64\bin\mingw32-make.exe"
set RC="C:\msys64\ucrt64\bin\windres.exe"

if not exist build mkdir build

echo.
echo [1/2] Configuring with CMake (MinGW)...
%CMAKE% -S . -B build -G %GENERATOR% ^
    -DCMAKE_C_COMPILER="C:/msys64/ucrt64/bin/gcc.exe" ^
    -DCMAKE_CXX_COMPILER="C:/msys64/ucrt64/bin/g++.exe" ^
    -DCMAKE_RC_COMPILER=%RC% ^
    -DCMAKE_MAKE_PROGRAM=%MAKE% ^
    -DCMAKE_BUILD_TYPE=Release

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] CMake configuration failed.
    pause
    exit /b 1
)

echo.
echo [2/2] Building...
%CMAKE% --build build --config Release

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

echo.
echo === Build successful! ===
echo Output: build\traffic_light.exe
echo.
echo Run it now? (Press any key to launch, Ctrl+C to cancel)
pause >nul
start "" "build\traffic_light.exe"
