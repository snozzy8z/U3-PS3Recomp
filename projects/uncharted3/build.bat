@echo off
setlocal enableextensions
cd /d "%~dp0"
set "SDK=..\.."
set "CFG=RelWithDebInfo"
set "LOG=%cd%\build.log"
echo ps3recomp build log - %DATE% %TIME% > "%LOG%"

echo.
echo === [1/4] Localisation de Visual Studio ===
set "VSINSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSINSTALL=%%i"
if not defined VSINSTALL if exist "C:\Program Files\Microsoft Visual Studio\18\Community" set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VSINSTALL if exist "C:\Program Files\Microsoft Visual Studio\2022\Community" set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not defined VSINSTALL goto no_vs
set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" goto no_vcvars
echo   VS: %VSINSTALL%
echo VSINSTALL=%VSINSTALL% >> "%LOG%"
call "%VCVARS%" x64 >> "%LOG%" 2>&1
where cl >nul 2>&1
if errorlevel 1 goto no_cl
where ninja >nul 2>&1
if errorlevel 1 goto no_ninja

echo.
echo === [2/4] Verification du code recompile ===
if not exist "recompiled\ppu_recomp_table.cpp" goto no_recomp
if not exist "game\EBOOT.ELF" echo   [AVERTISSEMENT] game\EBOOT.ELF absent : build OK mais run impossible.

echo.
echo === Cache de build ===
if not exist "build\CMakeCache.txt" goto configure
findstr /C:"CMAKE_GENERATOR:INTERNAL=Ninja" "build\CMakeCache.txt" >nul 2>&1
if errorlevel 1 goto wipe
echo   Cache Ninja existant -> build INCREMENTAL (rapide).
goto configure
:wipe
echo   Cache d'un autre generateur -> nettoyage complet.
rmdir /s /q build

:configure
echo.
echo === [3/4] Configuration CMake (Ninja, %CFG%) ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%CFG% -DPS3RECOMP_DIR="%SDK%" >> "%LOG%" 2>&1
if errorlevel 1 goto cfg_fail
echo   Configuration OK.

echo.
echo === [4/4] Compilation ===
echo   ~10,5 millions de lignes : 15 a 60+ min, peu d'affichage. Ne ferme pas.
echo   Progression dans build.log.
cmake --build build >> "%LOG%" 2>&1
if errorlevel 1 goto build_fail

echo.
echo ============================================================================
echo  BUILD REUSSI
echo  Binaire : build\Uncharted3Recomp.exe  (lance-le depuis ce dossier)
echo ============================================================================
goto end

:no_vs
echo [ERREUR] Visual Studio avec C++ introuvable.
goto end
:no_vcvars
echo [ERREUR] vcvarsall.bat introuvable sous "%VSINSTALL%".
goto end
:no_cl
echo [ERREUR] cl.exe indisponible apres vcvarsall (charge C++ manquante ?).
goto end
:no_ninja
echo [ERREUR] ninja introuvable (composant "C++ CMake tools for Windows").
goto end
:no_recomp
echo [ERREUR] recompiled\ppu_recomp_table.cpp absent : lance d'abord le lifting.
goto end
:cfg_fail
echo [ERREUR] Configuration CMake echouee. Ouvre build.log (cherche "CMake Error").
goto end
:build_fail
echo [ERREUR] Compilation echouee. Ouvre build.log et cherche " error " (avec espaces).
goto end

:end
echo.
pause
