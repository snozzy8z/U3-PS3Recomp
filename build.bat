@echo off
setlocal

echo ============================================
echo   ps3recomp - Build Script (Windows)
echo ============================================

:: Find Visual Studio
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do set VS_PATH=%%i

if not defined VS_PATH (
    echo ERROR: Visual Studio not found. Install VS 2022+ with C++ workload.
    exit /b 1
)
echo Found VS: %VS_PATH%

:: Configure if needed
if not exist build\CMakeCache.txt (
    echo.
    echo [1/2] Configuring CMake...
    cmake -B build -G "Visual Studio 18 2026" -A x64
    if errorlevel 1 (
        echo Trying VS 2022 generator instead...
        cmake -B build -G "Visual Studio 17 2022" -A x64
    )
    if errorlevel 1 (
        echo ERROR: CMake configure failed.
        exit /b 1
    )
)

:: Build
echo.
echo [2/2] Building...
cmake --build build --config Release
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo ============================================
echo   Build succeeded!
echo   Library: build\Release\ps3recomp_runtime.lib
echo ============================================
