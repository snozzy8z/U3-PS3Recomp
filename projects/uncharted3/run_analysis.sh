#!/usr/bin/env bash
# ============================================================================
#  Uncharted 3 -- Pipeline d'analyse ps3recomp (Linux/macOS)
#  Commandes alignees sur les CLI REELS des outils (verifies).
# ============================================================================
#  Prerequis : depose game/EBOOT.ELF (dechiffre) et game/PARAM.SFO
#  Depuis projects/uncharted3/ :  ./run_analysis.sh
# ============================================================================
set -e
SDK="../.."
ELF="game/EBOOT.ELF"

if [ ! -f "$ELF" ]; then
    echo "[ERREUR] $ELF introuvable. Depose ton EBOOT.ELF dechiffre dans game/"
    exit 1
fi
mkdir -p analysis spu_programs

echo "=== [1/3] Analyse de l'ELF (header, segments, imports, exports) ==="
python3 "$SDK/tools/elf_parser.py" "$ELF" --all > analysis/elf_info.json
echo "    -> analysis/elf_info.json"

echo "=== [2/3] Detection des fonctions ==="
python3 "$SDK/tools/find_functions.py" "$ELF" --json -o analysis/functions.json
echo "    -> analysis/functions.json"

echo "=== [3/3] Extraction des images SPU embarquees ==="
python3 "$SDK/tools/extract_spu_images.py" "$ELF" -o spu_programs/

echo ""
echo "=== Analyse terminee. Verifie analysis/ et spu_programs/ ==="
echo ""
echo "Etapes suivantes (a lancer manuellement quand pret) :"
echo "  # Lifting PPU (ELF + liste de fonctions -> code C dans recompiled/) :"
echo "  python3 $SDK/tools/ppu_lifter.py $ELF --functions analysis/functions.json -o recompiled/"
echo ""
echo "  # Pour chaque module .sprx du jeu, couverture/stubs :"
echo "  python3 $SDK/tools/prx_analyzer.py game/*.sprx --json --stubs"
echo ""
echo "  # Fonctions SPU pour chaque image extraite :"
echo "  python3 $SDK/tools/find_spu_functions.py spu_programs/spu_0.elf --out spu_disasm/spu_0.json"
