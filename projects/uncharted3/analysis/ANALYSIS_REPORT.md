# Uncharted 3 — Rapport d'analyse ps3recomp

Généré sur `EBOOT.ELF` (BCES01175, EU). Phase 1 du pipeline de portage.

## Identité du binaire

| Champ | Valeur |
|---|---|
| Jeu | Uncharted 3: Drake's Deception |
| Title ID | **BCES01175** (EU) |
| Format | ELF64, **big-endian**, PowerPC64 (Power ELF V1 ABI) |
| Type | Exécutable **statiquement lié**, **strippé** |
| Taille fichier | 18,8 Mo |
| Point d'entrée | `0x01179020` |
| Segment code (R-X) | vaddr `0x10000`, ~17,6 Mo |
| Segment data (RW) | vaddr `0x10E0000`, filesz ~1 Mo, memsz ~3,6 Mo |

> Comme tous les jeux Naughty Dog, le binaire est **statiquement lié** : pas de
> table d'imports PRX classique. Les appels système sont résolus via OPD/stubs,
> et le binaire étant strippé, il n'y a pas de noms de fonctions.

## Fonctions détectées

| Métrique | Valeur |
|---|---|
| **Fonctions totales** | **46 615** |
| Instructions désassemblées | 4 403 098 |
| Descripteurs OPD (points d'entrée sûrs) | 42 283 |
| Code couvert | ~14,4 Mo |
| Taille médiane d'une fonction | 112 octets |
| Blocs > 4 Ko (data/jump-tables potentiels) | 213 |

À titre de comparaison : flOw (petit titre PSN) avait ~51 000 fonctions. Uncharted 3
est dans le même ordre de grandeur côté nombre de fonctions, mais avec beaucoup
plus de code et de complexité SIMD (VMX/AltiVec) à lifter.

> ⚠️ Un « bloc » de ~2 Mo a été détecté comme une seule fonction : c'est très
> probablement de la donnée ou une grosse table mal classée. À inspecter avant
> le lifting (`ppu_disasm.py --offset ... --length ...`).

## Modules système référencés (37 familles)

D'après les chaînes du binaire (`analysis/system_symbols.txt`, 127 symboles) :

**Système / base** : sysPrxForUser, cellSysmodule, cellSysutil, cellSys*, cellFs,
cellRtc, cellGame, cellUser, cellSync, sys_ppu_thread_*, sys_lwmutex/lwcond,
sys_event_queue/port, sys_process, sys_io, sys_fs, sys_net.

**Graphisme** : cellGcm (RSX), cellVideo, cellResc.

**Audio** : cellAudio, cellAdec, cellVoice, cellSpurs (jobs).

**Entrées** : cellPad, cellUsbd.

**Image / vidéo** : cellPng, cellJpg, cellData.

**Sauvegarde / système** : cellSaveData, cellPhoto, cellRec.

**Réseau / PSN** : cellNet, cellHttp, cellSsl, sceNpInit, sceNpManager,
sceNpBasic, sceNpTrophy, sceNpProfile, sceNpMatching, sceNpCommerce, sceNpDrm,
sceNpSns, sceNpUtil.

> Le pan **PSN/réseau** est important (multijoueur + trophées). Pour un portage
> solo, on peut largement le stubber au début (sceNp* → `CELL_OK`/valeurs vides)
> et se concentrer sur le moteur.

## Programmes SPU — 8 images extraites (816 Ko)

C'est le point le plus encourageant : **chaque image SPU est identifiable**, et
toutes relèvent de catégories à stratégie HLE connue (remplaçables sur le CPU/des
libs hôte plutôt que d'interpréter le SPU intégralement).

| # | Offset | Taille | Rôle identifié (via chaînes) | Stratégie de port |
|---|--------|--------|------------------------------|-------------------|
| 0 | 0x01001500 | 137 Ko | **Physique** (`LtPhysics`, job system Naughty Dog) | Intercepter → CPU hôte |
| 1 | 0x01023800 | 164 Ko | **Physique** (`LtPhysics`, jobs) | Intercepter → CPU hôte |
| 2 | 0x0104C700 | 81 Ko | **Mixage audio** (BoomRang/BRB, `mixer_*`) | HLE → mixage hôte |
| 3 | 0x01060A80 | 132 Ko | **Mixage audio** (BRB, mixers multicanaux) | HLE → mixage hôte |
| 4 | 0x01081B80 | 115 Ko | **Décodage MP3** (BRB MP3 sampler) | HLE → décodeur hôte |
| 5 | 0x0109E800 | 104 Ko | **Décodage MP3** (BRB MP3 sampler) | HLE → décodeur hôte |
| 6 | 0x010B8800 | 28 Ko | **Audio jobs** (BRB buss context, DMA) | HLE → hôte |
| 7 | 0x010BF700 | 37 Ko | **EDGE Zlib** (décompression, `1.2.3.0-PS3-SPU-EDGE`) | Remplacer par zlib hôte |

**Bilan SPU : 5 images audio (mixage + MP3 + jobs), 2 physique, 1 décompression
zlib.** Aucune image de compute « exotique » détectée — c'est le scénario le plus
favorable pour un AAA. Le doc `SPU_FALLBACK.md` décrit exactement comment
intercepter ces tâches côté hôte.

## Couverture & risques

**Favorable** : SPU entièrement catégorisé et HLE-able ; ~46 k fonctions (gérable,
ordre de grandeur connu) ; modules système majoritairement standards.

**Chantiers majeurs (par ordre de difficulté décroissante)** :
1. **Graphisme RSX → D3D12/Vulkan** — le backend réel n'existe pas encore dans le
   SDK (seul `null` est prêt). C'est de loin le plus gros poste.
2. **SIMD VMX/AltiVec** — le moteur en abuse ; surveiller les instructions
   « unlifted » au lifting.
3. **Physique SPU** (`LtPhysics`) — réimplémenter/intercepter la logique de jobs.
4. **PSN/réseau** — stubbable au début, à implémenter pour le multi/trophées.

## Prochaines étapes immédiates

1. Installer `capstone`/`construct` (requis par le lifter) sur ta machine :
   `pip install -r tools/requirements.txt`
2. Lancer le lifting PPU :
   `python ..\..\tools\ppu_lifter.py game\EBOOT.ELF --functions analysis\functions.json -o recompiled\ -j 8`
3. Analyser les fonctions SPU :
   `python ..\..\tools\find_spu_functions.py spu_programs\spu_0007_at_010BF700.elf --out spu_disasm\spu_0007.json`
4. Lancer Uncharted 3 dans **RPCS3** avec logs « All » pour avoir la vérité terrain
   des séquences d'appels (cf. `GAME_PORTING_GUIDE.md` §Phase 0).
