# Uncharted 3: Drake's Deception — projet de portage ps3recomp

Scaffold de projet pour recompiler statiquement **Uncharted 3** vers PC natif
(Windows x86-64) avec ps3recomp.

> ⚠️ **Ampleur du projet.** La doc officielle classe *Uncharted* dans la
> catégorie **« AAA complexe — des mois à des années »**. Le moteur Naughty Dog
> est massivement basé sur les SPU (animation, physique, audio, compute) et son
> pipeline graphique RSX est complexe. Ce n'est pas un export en un clic : ce
> dossier te donne une base propre et une feuille de route, pas un binaire fini.

---

## Arborescence

```
uncharted3/
├── config.toml          # Config recompilateur (démarrage : null renderer, SPU on)
├── CMakeLists.txt        # Build du projet (lié au runtime ps3recomp)
├── main.cpp              # Point d'entrée runtime (template)
├── stubs.cpp             # Stubs/hooks spécifiques au jeu (à compléter)
├── run_analysis.bat      # Pipeline d'analyse (Windows)
├── run_analysis.sh       # Pipeline d'analyse (Linux/macOS)
├── game/                 # >>> DÉPOSE ICI EBOOT.ELF + PARAM.SFO + *.sprx <<<
├── analysis/             # Sorties d'analyse (imports, fonctions, NID résolus)
├── disasm/               # Désassemblage PPU annoté
├── recompiled/           # Code C/C++ généré (rempli par le lifter)
├── spu_programs/         # Programmes SPU extraits
├── spu_disasm/           # Désassemblage SPU
└── hdd0/                 # Filesystem virtuel PS3
    └── game/BCES01175/USRDIR/   # >>> COPIE ICI les assets du jeu <<<
```

> **Title ID** : `BCES01175` (EU) est utilisé par défaut. Si ta copie est US
> (`BCUS98233`) ou autre, renomme le dossier `hdd0/game/<TITLE_ID>/` en
> conséquence (le bon ID est dans `PARAM.SFO`).

---

## Ce qu'il te reste à fournir

1. **`game/EBOOT.ELF`** — l'EBOOT **déchiffré** en ELF (pas le `.BIN`/SELF chiffré).
   - Tu as dit l'avoir déjà : dépose-le dans `game/`.
2. **`game/PARAM.SFO`** — depuis `PS3_GAME/PARAM.SFO` du disque.
3. **`game/*.sprx`** — les modules additionnels depuis `PS3_GAME/USRDIR/`.
4. **Assets** — copie `PS3_GAME/USRDIR/*` dans `hdd0/game/<TITLE_ID>/USRDIR/`.

---

## Feuille de route (phase par phase)

### Phase 0 — Référence RPCS3 (à faire AVANT tout)
Lance Uncharted 3 dans RPCS3 avec `Log Level: All`. Note les modules chargés
(`cellSysmoduleLoadModule`), les NID fréquents, les warnings « Unimplemented »
et les patterns de création de threads SPU. C'est ta vérité terrain.

### Phase 1 — Analyse
```
run_analysis.bat
```
Examine ensuite `analysis/` :
- `elf_info.json` — header, segments, **imports** (modules + NID), exports
- `functions.json` — liste des fonctions détectées (attends-toi à **beaucoup** :
  flOw, un simple titre PSN, en avait déjà ~51 000)
- programmes SPU extraits dans `spu_programs/`

Pour la couverture des modules, lance `prx_analyzer.py` sur les `.sprx` du jeu
(`--json --stubs`) ; un AAA aura forcément des trous à combler dans `stubs.cpp`.

### Phase 2 — Lifting (génération du code C/C++)
Le lifter prend l'**ELF directement** + la liste de fonctions produite à la
phase 1 (`functions.json`), et écrit le code C dans `recompiled/` :
```
python ..\..\tools\ppu_lifter.py game\EBOOT.ELF --functions analysis\functions.json -o recompiled\ -j 8
```
Pour inspecter du désassemblage annoté d'une zone précise :
```
python ..\..\tools\ppu_disasm.py game\EBOOT.ELF --functions --json --offset 0x10000 --length 0x400
```
Puis ajoute les `.c`/`.cpp` générés à la liste des sources dans `CMakeLists.txt`.

> ⚠️ **AltiVec/VMX.** Le moteur Naughty Dog utilise massivement le SIMD VMX
> (vecteurs). Le lifter peut laisser des instructions « unlifted » (cf. notes
> flOw). Surveille ces compteurs : c'est souvent le premier gros chantier.

### Phase 3 — Build (Windows / VS2022)
D'abord, build le **runtime** ps3recomp une fois (depuis la racine du SDK) :
```
cd ..\..
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
Puis le projet :
```
cd projects\uncharted3
cmake -B build -G "Visual Studio 17 2022" -A x64 -DPS3RECOMP_DIR=..\..
cmake --build build --config Release
```
Attends-toi à des erreurs de link au 1er essai (fonctions manquantes,
conventions d'appel). C'est normal — corrige itérativement.

### Phase 4 — Premier run (null renderer)
`config.toml` est déjà réglé sur `backend = "null"` et
`break_on_unimplemented = true`. Lance, et traite les NID non implémentés un par
un dans `stubs.cpp` (cf. doc `GAME_PORTING_GUIDE.md` §8). Objectif : que la
logique du jeu et la boucle principale tournent **sans graphismes**.

### Phase 5 — SPU
Identifie le type de chaque tâche SPU (audio / physique / anim / décompression)
et privilégie l'approche HLE (intercepter et exécuter sur le CPU hôte) plutôt
que d'interpréter tout le code SPU. Voir `docs/SPU_LIFTER.md` et `SPU_FALLBACK.md`.

### Phase 6 — Graphismes (RSX → D3D12/Vulkan)
La partie la plus longue. Passe `backend` à `d3d12`, puis avance par étapes :
state tracking → triangle de base → translation des shaders RSX → textures →
framebuffers. Voir `docs/RSX_GRAPHICS.md`. **Note :** le backend D3D12/Vulkan
réel n'est pas encore fini côté SDK (seul le backend `null` l'est) — prévois du
travail d'implémentation côté renderer.

### Phase 7 — Audio / Input / Sauvegardes / Trophées / Polish
WASAPI + XInput sont déjà câblés dans la config. Reste : `cellSaveData`,
trophées (`sceNp`), réglages de perf (`-O2`, désactiver `log_hle_calls`).

---

## Docs SDK à garder sous la main
- `docs/GAME_PORTING_GUIDE.md` — le guide complet (réfère-toi y en continu)
- `docs/MODULE_STATUS.md` — ce qui est couvert en HLE
- `docs/NID_SYSTEM.md` — résolution des NID / stubs
- `docs/SPU_LIFTER.md`, `docs/SPU_FALLBACK.md` — SPU
- `docs/RSX_GRAPHICS.md` — graphismes
- `docs/SYSCALLS.md` — syscalls

## Légal
N'utilise que ta **copie légalement acquise** du jeu et tes propres clés.
Ne redistribue ni l'ELF, ni les assets, ni le binaire recompilé.
