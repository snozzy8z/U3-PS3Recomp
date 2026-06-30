# Step 5 — Lifting PPU → C  (à exécuter sur ta machine Windows)

## État : CODE GÉNÉRÉ ✓ (dans `recompiled/`)

Le code C++ a bien été **généré entièrement** ici, et un échantillon **compile**
(`g++ -fsyntax-only`, rc=0, contre `ppu_recomp.h` + les includes du SDK).

| Métrique | Valeur |
|---|---|
| Fonctions de base | 46 615 |
| Entrées mid-fonction (points d'entrée internes) | 36 416 |
| **Total dans `function_table[]`** | **83 031** |
| Fichiers générés | 64 × `.cpp` + `ppu_recomp.h` + `ppu_recomp_table.cpp` |
| Lignes de C++ | ~10,5 millions |
| Taille sur disque | ~756 Mo |

Fichiers dans `recompiled/` : `ppu_recomp_b0000.cpp … b0023.cpp` (lift de base,
2000 fn/fichier), `ppu_recomp_mid0001.cpp …` (entrées mid-fonction),
`ppu_recomp.h` (déclarations + types `ppu_context`/`ppu_vr`), et
`ppu_recomp_table.cpp` (la table `function_table[]` combinée).

### Comment ça a été généré : `tools/stream_lift.py`

Le `ppu_lifter.py` d'origine garde tout en mémoire puis sérialise ~10 M de
lignes d'un coup — trop long pour la limite de 45 s/commande de cet
environnement. J'ai donc écrit **`tools/stream_lift.py`**, un pilote *streaming*
et *reprenable* : il lifte par lots, écrit chaque lot sur le disque
immédiatement (mémoire libérée), et reprend là où il s'est arrêté (état dans
`recompiled/.stream_state.json`). Il reproduit l'algo d'origine : mêmes noms
`func_<ADDR>`, même preamble, trampolines de fallthrough corrects en frontière
de lot, entrées mid-fonction régénérées, table `function_table[]` combinée.

```bat
:: relancer tant que ça n'affiche pas "ALL DONE" (reprend automatiquement)
python ..\..\tools\stream_lift.py game\EBOOT.ELF ^
    --functions analysis\functions.json -o recompiled\ ^
    -j %NUMBER_OF_PROCESSORS% --batch 2000 --mid-chunk 1000 --time-budget 600
```

> Sur ta machine (pas de limite de temps), mets `--time-budget` à une grande
> valeur : un seul appel suffira (quelques minutes).

> Alternative « officielle » équivalente (un seul process, sans reprise) :
> `python ..\..\tools\ppu_lifter.py game\EBOOT.ELF --functions analysis\functions.json -o recompiled\ -j 8`
> — produit `ppu_recomp_000.cpp …` + table dans le dernier chunk. Les deux
> sorties sont compatibles avec le CMake du projet (glob `recompiled\*.cpp`).

## Commande à lancer (Windows, depuis `projects\uncharted3\`)

Les outils d'analyse/lift n'ont **pas** besoin de capstone (stdlib seule). Mais
installe quand même les deps pour les autres outils :

```bat
pip install -r ..\..\tools\requirements.txt
```

Puis lance le lift (≈ quelques minutes, sort dans `recompiled\`) :

```bat
python ..\..\tools\ppu_lifter.py game\EBOOT.ELF ^
    --functions analysis\functions.json ^
    -o recompiled\ ^
    -j %NUMBER_OF_PROCESSORS%
```

Sortie attendue dans `recompiled\` :
- `ppu_recomp.h` — déclarations (forward decls de toutes les fonctions)
- `ppu_recomp_000.cpp`, `ppu_recomp_001.cpp`, … — chunks de ~600 k lignes
  (découpés sous la limite MSVC ; le dernier chunk porte la `function_table[]`)

> Astuce : `-j` à fond utilise tous les cœurs. Avec un CPU récent (8–16 cœurs),
> compte ~1–3 min pour le lift complet, bien plus rapide qu'ici (2 cœurs).

## Step 6 — Build (Windows / VS2022)

1) Build du **runtime** ps3recomp une fois (depuis la racine du SDK) :
```bat
cd ..\..
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
```
2) Build du projet (le CMake inclut déjà `recompiled\*.cpp` via glob + `/bigobj`) :
```bat
cd projects\uncharted3
cmake -B build -G "Visual Studio 17 2022" -A x64 -DPS3RECOMP_DIR=..\..
cmake --build build --config RelWithDebInfo
```

> Pour le **bring-up**, build en `Debug`/`RelWithDebInfo` (pas `Release`) :
> compiler 3M lignes en `/O2` est très long. Passe en `Release` une fois stable.

## ⚠️ Point d'intégration à corriger (template ↔ code généré)

Le template et le code généré ne nomment pas la table pareil :

| | Template (`main.cpp`/`stubs.cpp`) | Code généré (lifter) |
|---|---|---|
| Table | `g_recompiled_funcs[]` / `g_recompiled_func_count` | `function_table[]` / `function_table_count` |
| Entrée | `recomp_game_main(ctx)` | fonctions `func_<ADDR>(ppu_context*)` |

Donc dès que le vrai code est généré :
1. **Retire** de `stubs.cpp` la table placeholder `g_recompiled_funcs[]`, le
   `g_recompiled_func_count = 0` et le faux `recomp_game_main` (sinon doublons /
   table vide).
2. Adapte `main.cpp` pour : charger `function_table[]` dans le dispatcher du
   runtime, puis entrer au **point d'entrée réel** `0x01179020` (fonction
   `func_01179020`) au lieu de `recomp_game_main`.
3. Fournis les symboles attendus par le code généré (normalement dans le
   runtime/stubs) : `vm_base`, `ps3_indirect_call`, `g_trampoline_fn`.

Voir `docs/RUNTIME.md` (dispatcher, contexte PPU) et la section « HLE Bridge
Pattern » du `GAME_PORTING_GUIDE.md`.

## Step 7 — Premier run

`config.toml` est déjà en `backend = "null"` + `break_on_unimplemented = true`.
Lance le binaire, et traite les `[HLE] UNIMPLEMENTED` un par un dans `stubs.cpp`.
Objectif de cette phase : faire tourner la logique + boucle principale **sans
graphismes**, en t'appuyant sur RPCS3 (logs « All ») comme vérité terrain.
