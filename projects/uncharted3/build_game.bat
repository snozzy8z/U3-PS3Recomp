@echo off
:: Build Uncharted3 recomp project with Ninja + MSVC.
:: Sets up the MSVC environment (vcvars64) and adds VS's bundled Ninja to PATH.
setlocal

set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "NINJA_DIR=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

call "%VCVARS%"
set "PATH=%NINJA_DIR%;%PATH%"

cd /d "%~dp0"

:: Configure if no build dir
if not exist build\build.ninja (
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS3RECOMP_DIR=..\..
)

:: Build (resumes from where it left off). Limit parallel compiles: the
:: generated TUs are huge (100k-700k lines) and each cl can use several GB; the
:: default (#cores+2) jobs exhaust RAM (C1002) on a 16 GB box, especially with
:: RPCS3 open. 4 concurrent compiles is a safe ceiling here.
if "%*"=="" (
    cmake --build build -j 4
) else (
    cmake --build build %*
)
