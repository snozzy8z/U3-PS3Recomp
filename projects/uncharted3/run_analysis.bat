@echo off
REM ============================================================================
REM  Uncharted 3 -- Pipeline d'analyse ps3recomp (Windows)
REM  Commandes alignees sur les CLI REELS des outils (verifies).
REM ============================================================================
REM  Prerequis : depose game\EBOOT.ELF (dechiffre) et game\PARAM.SFO
REM  Depuis projects\uncharted3\ :  run_analysis.bat
REM ============================================================================
setlocal
set SDK=..\..
set ELF=game\EBOOT.ELF

if not exist "%ELF%" (
    echo [ERREUR] %ELF% introuvable. Depose ton EBOOT.ELF dechiffre dans game\
    exit /b 1
)
if not exist analysis md analysis
if not exist spu_programs md spu_programs

echo === [1/3] Analyse de l'ELF (header, segments, imports, exports) ===
python "%SDK%\tools\elf_parser.py" "%ELF%" --all > analysis\elf_info.json
echo     -^> analysis\elf_info.json

echo === [2/3] Detection des fonctions ===
python "%SDK%\tools\find_functions.py" "%ELF%" --json -o analysis\functions.json
echo     -^> analysis\functions.json

echo === [3/3] Extraction des images SPU embarquees ===
python "%SDK%\tools\extract_spu_images.py" "%ELF%" -o spu_programs\

echo.
echo === Analyse terminee. Verifie analysis\ et spu_programs\ ===
echo.
echo Etapes suivantes (a lancer manuellement quand pret) :
echo   REM Lifting PPU (ELF + liste de fonctions -^> code C dans recompiled\) :
echo   python "%SDK%\tools\ppu_lifter.py" "%ELF%" --functions analysis\functions.json -o recompiled\
echo.
echo   REM Pour chaque module .sprx du jeu, couverture/stubs :
echo   python "%SDK%\tools\prx_analyzer.py" game\*.sprx --json --stubs
echo.
echo   REM Fonctions SPU pour chaque image extraite :
echo   python "%SDK%\tools\find_spu_functions.py" spu_programs\spu_0.elf --out spu_disasm\spu_0.json
endlocal
