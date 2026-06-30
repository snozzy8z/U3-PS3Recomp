# Uncharted 3 (BCES01175) — Roadmap long terme vers la jouabilité

But : amener le portage recomp d'un état « boote + fenêtre + géométrie de menu »
à « jouable ». Effort multi-mois. Ce doc est le PLAN durable : chaque session
exécute une étape, met à jour STATUS_AND_ROADMAP.md (journal détaillé) et coche ici.

## État acquis (juin 2026) — voir STATUS_AND_ROADMAP.md pour le détail
- [x] Boot moteur complet, PRX, SPURS init, chargement des vrais niveaux enumeres.
- [x] Fenetre D3D12 reelle, pipeline FIFO RSX -> backend (clears, etat, shaders vus).
- [x] Geometrie du menu (immediate-mode inline) RENDUE (quads visibles).
- [x] Input XInput branche (cellPad polling).
- [x] 8 images SPU liftees (7414 fns) + harnais d'execution (UC3_SPU_TASK).
- [x] Decompression Edge zlib HLE (LFQueuePush).

## Verrou central = EXECUTION SPU/SPURS (tout converge ici)
Le jeu n'avance pas (pas d'assets -> pas de textures -> pas de gameplay) parce que
les jobs SPU ne tournent pas. Le manager spu_0007 diverge car la table de dispatch
en LS@0xBEC0 doit etre posee par le runtime du kernel SPURS. Sans SPU : pas de
geometrie 3D, pas de streaming d'assets, pas de progression de jeu.

---

## RE OPTION 1 - PROGRES 2026-06-30 : bloc de controle taskset cartographie
Dump (UC3_DUMP_RING) de l'arg du workload (0x3077F380) = bloc de controle taskset SPURS :
  +0x00 : 0x010D8C00 (policy module addr) ; +0x04 : 0x00002F80 (taille policy = 12KB)
  +0x20 : 0x000001FF (flags/count) ; +0x30 & +0x80 : ptr 0x3077F480 (donnees taskset)
  +0x40..0x70 : table de slots 16o (mot=0 utilise / 0xFFFFFFFF libre) = slots taches/jobs
  +0x80 : 0x3077F480, 0x00000200, 0x012FC730 (objet fence, cf [spu-fence])
Le sub-bloc @0x3077F480 = TOUT ZERO au moment du CreateTask (init). 
=> LE RING DE JOBS EST VIDE A L'INIT. Les jobs geometrie sont soumis PLUS TARD,
PAR FRAME, pendant le rendu (pas a CreateTask). Le point d'interception = la
soumission per-frame dans la queue taskset (~0x3077F380/0x3077F480).
CAPTURE per-frame (UC3_RING_MON, thread moniteur sur le bloc de controle) :
le PPU SOUMET du travail par frame -> les compteurs/bitsets du taskset s'incrementent :
  +0x40 : 00000000 -> 00000001  (compteur slot 0 = running/ready bitset, 1 job soumis)
  +0x88 : 00000002  (2e compteur)  ; +0x80 ptr 0x3077F480, fence 0x012FC730
Donc la soumission de travail per-frame est CONFIRMEE et OBSERVABLE ici. La boucle de
rendu tourne ([gcm-overflow]).
JOBS LOCALISES 2026-06-30 : quand le compteur s'incremente, le sub-bloc 0x3077F480 se
remplit de descripteurs (paires mot+pointeur) :
  desc=0x00300001 ptr=0x20E1E380 -> 00 00 02 00 10 05 01 00 ... 04 58 A4 00 00 EF FA 00 ... 20 E1 E3 B0
  desc=0x00600001 ptr=0x20E42E00 -> 00 00 02 00 10 0F 01 00 ... 06 C4 A4 00 00 FF EC 80
=> FORMAT = job SPURS (Job2/JobChain) : en-tete (0x00000200=taille binaire), listes DMA
I/O (paires taille+EA, ex 0x00EFFA00), ptr de continuation (0x20E1E3B0). Les ptr
0x20Exxxxx (region overlay, cf lr world-lookups 0x20FA6930) pointent le code/data du job.
=> QUEUE DE JOBS + FORMAT DESCRIPTEUR LOCALISES. C'est le coeur de l'option 1.
PARSING DESCRIPTEUR (2026-06-30) : desc=0x00300001 -> 0x0030 = 48 = taille du
CellSpursJobHeader ; le ptr (0x20E1E380) pointe un header de 48o :
  00 00 02 00 10 05 01 00 00 00 00 00 15 01 01 00 04 58 A4 00 00 EF FA 00 00 04 E4 20 20 E1 E3 B0
EA candidates dans le header (0x00EFFA00, 0x0458A400, 0x0004E420) NE matchent PAS nos
images extraites (guest 0x0101xxxx+). => le CODE du job (eaBinary) est ailleurs
(overlay region 0x20Exxxxx, NON extrait/lifte).
EXECUTEUR DEMARRE 2026-06-30 : policy module extrait+lifte pour RE du format.
- policy_module.bin (0x2F80=12160 o @ guest 0x010D8C00, taille lue au +0x04 du bloc
  controle taskset). Lifte dans spu_gen/policy/ (base 0xA00 = workload entry).
- Le format CellSpursJobHeader/Edge N'EST PAS PUBLIC (2 web searches infructueuses :
  psdevwiki/github n'ont pas le layout exact). DONC : le RE doit se faire depuis le
  DESASM du policy module (il parse les descripteurs : trouver les loads sur le job
  ptr -> offset de eaBinary + DMA list) ET/OU les descripteurs captures (UC3_RING_MON).
- eaBinary pointe vers overlay 0x20Exxxxx (NON extrait) -> a lifter aussi.
PERCEE 2026-06-30 : LES BINAIRES DE JOB EDGE SONT STATIQUES DANS L'EBOOT + LIFTABLES.
Capture live (UC3_RING_MON, dump 256o + scan EA peuplees) : les descripteurs de job
referencent des binaires SPU EDGE POPULES en mémoire principale (dans la plage EBOOT) :
  job1 desc@0x20E1E380 : binaire @0x00EFFA00 (file 0xEEFA00) - format Edge job :
    +0x00 code SPU(ila) ; +0x10=0xEA0/+0x14=0x1180 (tailles) ; +0x20 C0DEC0DE (magic) ;
    +0x28=0x00004000 (LOAD ADDR LS) ; +0x30 = code SPU reel.
  job2 desc@0x20E42E00 : binaire @0x00FFEC80 (idem, C0DEC0DE@+0x20, load 0x4000).
  (champ +0x18 d'un desc = 0x0004E420 = code PPC = callback PPU, pas le binaire SPU.)
=> Ma conclusion "code overlay non extractible" etait FAUSSE. Les binaires Edge sont
DANS l'EBOOT (statiques), avec header C0DEC0DE + load@0x4000. EXTRACTIBLES + LIFTABLES.
FAIT : extrait edge_job_EFFA00.bin (code @+0x30), wrap ELF (base/entry 0x4000), lifte
spu_gen/edge0/ : 58 fonctions, 86%, entry spu_func_00004000. C'EST LE CODE DE JOB
GEOMETRIE, PRET.
=> L'EXECUTEUR EST DEBLOQUE (plus de ressource manquante). RESTE (build-out faisable) :
  1. RE l'ABI de demarrage de job (registres+LS que le policy pose avant bisl@0x32AC).
  2. Parser le descripteur : eaJobBinary @+0x48(?)=EA / dmaList (inputs) - offsets a
     finaliser (les 2 jobs ont des layouts differents -> c'est du DMA-list, un champ
     = le binaire, d'autres = inputs vertex).
  3. Harnais : spu_context, charger binaire Edge @LS 0x4000, DMA inputs, run
     spu_func_00004000, outputs DMA -> FIFO -> backend D3D12 (deja cable).
Artefacts: spu_programs/edge_job_EFFA00.{bin,elf}, spu_gen/edge0/. Le format Edge job
(C0DEC0DE) permet d'extraire TOUS les binaires de job de l'EBOOT (scan C0DEC0DE).

ABI DEMARRAGE JOB (policy disasm @0x31FC-0x32AC, avant bisl $r5) : COMPLEXE. Le policy
remplit de nombreuses zones LS = bloc de parametres du job : stqa vers 0x4A80, 0x4AC0,
0x4B40, 0x4B80, 0x4C80, 0x4D00, 0x4E40, 0x4E80, 0x4FC0, 0x5640, 0x5880, 0x58C0 + brsl
0x2618/0x2648/0x35A0 (sous-routines de setup) ; r5 = entry du job charge. Le job Edge
lit ses inputs depuis ces zones LS. => Repliquer l'executeur = reproduire ce bloc de
parametres + DMA les inputs (dmaList) + run spu_func_00004000 + outputs->FIFO. INTRIQUE
mais FAISABLE. C'est le build-out restant (multi-etapes, plusieurs sessions).
ETAT : etape SPU DEBLOQUEE (binaires extractibles+liftes, format decode, ABI localisee)
mais l'executeur fonctionnel (geometrie rendue correcte) reste un build intriqué.

EXECUTEUR CONSTRUIT + TOURNE 2026-06-30 : 
- Build bascule sur edge0 (Edge job) au lieu de spu_0007 (manager) - modele doc
  "run jobs not manager". CMakeLists SPU_LIFTED_SOURCES = spu_gen/edge0/edge0.c.
- stubs.cpp : uc3_run_edge_job(ea_binary) : valide C0DEC0DE @+0x20, charge code (+0x30)
  en LS @load_addr (0x4000), SP=0x3F000, run spu_func_00004000 + garde. Hook depuis le
  ring monitor (UC3_EDGE_JOB) quand un champ descripteur pointe un binaire C0DEC0DE.
- Fix lifter : collecte des cibles ila/splat (sauts indirects) -> re-lift edge0 avec
  --functions (87 fns, +29 seeds) -> spu_func_00004058 etc. resolus.
RESULTAT : le job Edge s'EXECUTE PROPREMENT (status RUNNING, 0 branche inconnue, pas
de crash) ! Il RETOURNE TOT (pc=0x4058, 0 DMA) car son BLOC DE PARAMETRES est vide :
il lit LS@0x5130 (pointeur de params) = 0 -> sortie anticipee (pas de travail).
=> L'INFRA DE L'EXECUTEUR FONCTIONNE END-TO-END. RESTE (piece finale) : peupler le
bloc de params Edge en LS (0x5000-0x5140) + les inputs DMA du dmaList du descripteur.
BLOC PARAMS LOCALISE (policy disasm) : le policy ecrit le bloc params depuis des
registres derives du descripteur :
  stqa r11->0x5000 ; r6->0x5040 ; r3->0x5080 ; r4->0x50C0 ; r5->0x5140.
Le job edge0 lit 0x50B0,0x50C0,0x50D0,0x50E0,0x50F0,0x5110(x6),0x5130,0x5140.
=> PIECE FINALE : tracer r3/r4/r5/r6/r11 (policy) jusqu'aux champs du descripteur pour
repliquer le bloc params (~5 valeurs) + DMA les inputs (dmaList) en LS, puis le job
traite la geometrie -> DMA outputs -> FIFO -> backend D3D12. Intriqué (RE per-param)
mais BORNE et localise. ALTERNATIVE : run le policy module lifte (spu_gen/policy/)
mais il faut son env kernel. C'est la derniere ligne droite de l'etape SPU.

CONSTAT 2026-06-30 : le bloc params N'EST PAS une simple copie de champs. Le policy le
BATIT via une logique COMPLEXE (policy disasm @0x2BA0-0x32AC) : r6->0x5040=shlqbyi(r4,4)
(r4=ptr job) ; r3->0x5080 = resultat de SOUS-ROUTINES (brsl 0x26D0, 0x3640, 0x3650) ;
r5/r4/r11 idem. Donc repliquer le bloc params = RE de plusieurs sous-routines policy OU
executer le policy job-start lifte (besoin du descripteur en LS + r4 + env SPURS). LA
DERNIERE LIGNE DROITE EST GENUINEMENT MULTI-SESSIONS. Voie prochaine session : lifter
proprement le policy (find_spu_functions) + run la sequence job-start sur un descripteur
capture (UC3_RING_MON) pour generer le bloc params, puis lancer edge0.

RE DESASM POLICY MODULE 2026-06-30 (spu_disasm policy_module.bin --base 0xA00, dump
scratchpad/policy_disasm.txt, 3040 instr) : mecanisme d'exec job LOCALISE :
- workload entry @0xA00 -> setup stack (lqa 0x5600) -> br 0x2BA0 (logique).
- DMA du binaire de job @0x2018 : MFC_LSA=r6(calcule), MFC_EAL=r5 ou r5 =
  andi(rotqbyi(r29,4),-16) -> eaBinary = champ du descripteur (r29, mot @+4),
  MFC_Cmd=0x40 (GET), size=r3 (rotmi). Le descripteur est dans r29.
- Dispatch du job @0x32AC : bisl $r5 (saut vers le code de job charge en LS).
=> Le policy module : charge descripteur -> extrait eaBinary (r29) -> DMA binaire job
   -> bisl. Adresses cles: DMA@0x2018, dispatch@0x32AC, entry@0xA00->0x2BA0.
TRACE r29 (2026-06-30) : @0x1DAC lqd $r29,0($r28) + rotqby $r29,$r29,$r28 -> r29 =
chunk 16o du descripteur en LS[r28] (realigne par bits bas de r28). @0x1FDC
eaBinary = rotqbyi(r29,4).word0 & ~0xF = WORD1 (octets +4..+7) du chunk descripteur.
=> eaBinary @ offset +4 d'un chunk descripteur a LS[r28]. PROCHAIN RE : tracer r28
(adresse LS du descripteur, DMA depuis main) pour l'offset absolu ; idem pour la DMA
list des inputs (autres champs). NOTE: RE incrementale lente (flow multi-fonction SPU) ;
chaque champ = plusieurs lectures de disasm. L'executeur complet reste multi-semaines.
ANCIEN PREREQUIS : le format binaire CellSpursJob2/CellSpursJobHeader vient du
SDK Cell (PAS dans les headers RPCS3 - RPCS3 HLE les jobchains mais pas le format job).
=> Trouver le format CellSpursJobHeader (SDK Cell / PSL1GHT / homebrew headers) pour
parser eaBinary + DMA list I/O. Puis : extraire+lifter le code de job pointe par
eaBinary (overlay 0x20Exxxxx), DMA les I/O dans le LS, executer, DMA outputs->FIFO.
C'est le prochain gros chantier (executeur Job2), bien defini mais conséquent.
ANCIEN: implementer l'EXECUTION SPURS Job2 (ref RPCS3 cellSpursJobQueue/job2).
ANCIEN: trouver les DESCRIPTEURS de job que ces compteurs referencent. Pistes : suivre le ptr +0x80 (0x3077F480) quand
le compteur s'incremente (il restait 0 a l'init mais peut se remplir per-frame) ;
ou tracer cote PPU la fonction qui incremente +0x40 (write-watch sur warg+0x40) pour
remonter au code qui ecrit le descripteur + son adresse. Puis : executer la fonction
liftee spu_0000/0001 avec ce descripteur -> DMA vertices+DRAWs dans le FIFO -> backend.
NOTE: policy module 0x010D8C00 taille 0x2F80 (confirme). Outils: UC3_DUMP_RING (dump
init), UC3_RING_MON (moniteur per-frame) dans stubs.cpp br_cellSpursCreateTask.

## CORRECTION DE CAP 2026-06-30 (re-lecture docs/FAQ.md + Phase 11 + SPU_FALLBACK.md)
Le MODELE de la doc : (1) HLE de la gestion cellSpurs SANS executer le SPU
(FAQ: "SPURS management - cellSpurs handles task scheduling without actual SPU
execution") ; (2) les JOBS de contenu sont HLE par type OU liftes (SPU lifter).
=> NE PAS faire tourner le MANAGER spu_0007 (c'est de la gestion -> a HLE). Faire
tourner/HLE les JOBS : spu_0000/0001 (geometrie, deja liftes dans spu_lifted/).
Mon travail 29t-30 (faire executer le manager spu_0007 + reproduire le kernel
SPURS en LS) etait le chemin le PLUS DUR (kernel firmware) et HORS modele doc -
garde comme reference mais ce n'est pas la voie prescrite.
NOUVELLE VOIE A (doc-alignee) :
 A1'. HLE l'ordonnancement cellSpurs : intercepter la soumission de job geometrie
      (UC3 = modele Taskset ; le manager pull les jobs depuis une ring/queue que le
      PPU remplit). Trouver cette ring (cote PPU) = le point d'interception.
 A2'. Pour chaque job geometrie soumis, executer la fonction liftee spu_0000/0001
      avec le descripteur du job (input depuis la ring) -> son DMA ecrit
      vertices+DRAWs dans les segments FIFO -> backend D3D12 (deja cable) -> 3D.
 DIFFICULTE RESIDUELLE : trouver la ring de jobs sans executer le manager (qui la
 lit en interne). Options : (a) tracer le PPU autour de la soumission ; (b) capturer
 via RPCS3 ce que le PPU ecrit. C'est plus aligne doc que le HLE-kernel, mais encore
 du RE. NOTE: la decompression (Edge zlib) suit deja ce modele (LFQueuePush HLE).

## PHASE A — Execution SPU/SPURS (chemin critique, le plus dur) [APPROCHE INITIALE, hors-modele]

### A0. Obtenir l'ABI SPURS (PREREQUIS, prochaine action)
Sans le layout LS du kernel SPURS, A1 est du devinage. Sources possibles :
- Source RPCS3 : `rpcs3/Emu/Cell/Modules/cellSpurs.h` (structures main-memory :
  CellSpurs, CellSpursTaskset, CellSpursWorkloadInfo...) + `cellSpurs.cpp` /
  `cellSpursSpu.cpp` (logique kernel SPU, le layout LS : kernel context, taskset
  context, dispatch). C'est LA reference.
- Ou dump du Local Store reel d'un SPU via RPCS3 (debugger) — peu praticable via UI.
Action concrete : recuperer cellSpurs.h + cellSpursSpu.cpp depuis github RPCS3,
extraire le layout LS (offsets du kernel/taskset context, ou va la dispatch table).

### A0 RESULTAT (2026-06-29) — ABI SPURS obtenue (RPCS3 cellSpursSpu.cpp)
Layout LS du kernel SPURS :
- Kernel context @ 0x100 (SpursKernelContext : spuNum, spurs ptr, dmaTagId, exit addr)
- Taskset context @ 0x2700 (SpursTasksetContext : taskId, syscall addr, DMA tag, regs)
- Task info @ 0x2780 ; Workload info @ 0x3FFE0 (20B, copie de la main memory)
- Trace @ 0x2C00 ; workload state copy @ 0x2D80
- STACK POINTER = 0x3FFB0 ; kernel ctx addr = 0x100 ; workload entry = 0xA00
ABI d'entree de tache (spursTasksetStartTask) :
- gpr[1] = SP = 0x3FFB0   <-- CRITIQUE (mon harnais mettait 0 -> corruption)
- gpr[2] = 0
- gpr[3] = task args (lower 64-bit)
- gpr[4]._u64[0] = taskset->spurs addr ; gpr[4]._u64[1] = taskset->args
- gpr[5..127] = 0 ; pc = ELF entry
Diagnostic confirme : avec gpr[1]=0, le prologue ls_write128(gpr[1]-0x20,...) ecrit en
LS[0x3FFE0] (workload info) et SP devient garbage -> divergence. Fix = poser gpr[1].

### A1. HLE du kernel SPURS : poser l'environnement de tache en LS — FAIT (2026-06-29)
Implemente dans stubs.cpp (uc3_spu_task_run + br_cellSpursCreateTask) :
- gpr[1]=SP=0x3FFB0, gpr[3]=task args, gpr[4]=spurs/taskset args.
- Kernel context @LS 0x100 : spurs ptr @0x1C0 (low @0x1C4), spuNum@0x1C8, dmaTagId
  @0x1CC, wklCurrentId@0x1DC.
- Taskset context @LS 0x2700 : taskset ptr @0x27B8 (low @0x27BC), spuNum@0x27CC,
  dmaTagId@0x27D0, taskId@0x27D4.
- Main-memory CellSpursTaskset peuple : spurs@0x60, enabled bit (taskset+0x30),
  ready bit (+0x10), task_info[0]@0x80 (args@0/elf@0x10/ctx_save@0x18/ls_pattern@0x20).
RESULTAT : plus de divergence (0 branches inconnues) ; le manager s'execute LONGUEMENT
(100k+ DMA) avec cet environnement. A1 FONCTIONNELLEMENT FAIT.

### A2. Faire dispatcher le manager -> jobs (ETAPE COURANTE)
ETAT (2026-06-29) : avec A1, le manager s'execute mais entre dans une BOUCLE
DEGENEREE (trace DMA : PUT lsa==ea size=2 incrementiel, 100k+ fois -> debordement
de pile hote -> segfault, chemin gate UC3_SPU_TASK uniquement ; build par defaut OK).
=> Le manager traite des donnees de JOB INVALIDES : la QUEUE DE JOBS dynamique (le
travail que le PPU pousse au manager) n'est pas peuplee. A1 a pose l'ENVIRONNEMENT
STATIQUE ; A2 doit fournir le TRAVAIL.
A FAIRE :
- Comprendre le protocole de queue de jobs SPURS (cellSpursJobQueue / la ring que le
  manager DMA). Ref RPCS3 cellSpurs.cpp / cellSpursJobQueue + le dump PS3 (lire la
  structure CellSpurs @0x21AF3E80 -base +0x2BF0000 dans le dump pour voir les workload
  info / job queues reels).
- Soit fournir un vrai job descriptor (depuis ce que le PPU a soumis), soit detecter
  "pas de job" et faire sortir le manager proprement (au lieu de boucler sur garbage).
- Garde anti-debordement : detecter la boucle degeneree et halt le SPU (setjmp/longjmp
  comme le harnais de test SPU, cf docs/SPU_LIFTER.md) pour que le chemin gate ne
  crashe plus.
- Charger le code du job (spu_0000/0001...) dans le LS (overlay) + bascule image_id
  (spu_begin_image, registry par image deja dans spu_channels.c) + execution.

DECOUVERTE 2026-06-29 (A2) : peuple aussi la workload info @LS 0x3FFE0 (AddWorkload
ecrit CellSpurs.wklInfo1[wid]@spurs+0xB00 : addr=pm=0x010D8C00, arg=0x3077F380 =
base taskset/fence reelle ; harnais copie 20B vers LS@0x3FFE0). MAIS le 1er DMA de la
tache reste ea=0 : le task entry (0x3050) lit un pointeur depuis LS@0xC400 (et 0xBEC0)
que le POLICY MODULE aurait pose AVANT de demarrer la tache. On saute le policy module.
=> CHEMIN PROPRE A2 : LIFTER + EXECUTER le policy module pm=0x010D8C00 (code SPU brut
SDK, PAS un ELF EM_SPU - extraire un chunk a 0x010D8C00, lift via spu_lifter
--offset/--base, run comme workload entry @0xA00 avec gpr[3]=0x100 kernel ctx) au lieu
de jumper a la tache. Le policy module fait tout le setup LS (0xC400, dispatch) +
demarre la tache correctement. Alternative (fragile) : RE de chaque pointeur lu par la
tache. Le dump PS3 peut donner les valeurs LS reelles (0xC400, 0xBEC0 post-policy) si
on capture le LS via RPCS3 (non fait). NOTE: spu_0007 (image extraite) contient
peut-etre DEJA le policy module (c'est la tache runner) -> verifier si 0xA00 est une
fonction liftee et si l'entry kernel/workload y est.

### A3. Jobs geometrie -> FIFO
Les jobs Edge (spu_0000/0001) ecrivent vertices+DRAWs par DMA dans les segments
FIFO reserves. Notre walker FIFO (stubs.cpp) + backend D3D12 les consomment deja.
Test : draws != 0 ; geometrie 3D dans la fenetre.

### A4. Signalisation de completion -> deblocage PPU
Les jobs signalent events/flags/counters -> le scheduler PPU (func_00038FBC)
avance -> flip. Test : SetFlipCommand appele, le jeu progresse (charge des assets).

---

## PHASE B — RSX completion (apres A, pour le visuel reel)
### B1. Texture upload
La VRAM se peuple (via assets decompresses, debloque par A). Implementer
d3d12_bind_texture : creer ressource (BC1/ARGB8), upload depuis vm_base+0xC0000000+
offset (loc=1) ou main (loc=2), SRV heap, sampler, PSO texture, capture texcoord
(deja faite dans inline_verts). Format DXT1=0x86 confirme pour l'atlas menu.
### B2. Shader translation (fragment/vertex programs -> HLSL)
rsx_fp_decompiler/rsx_vp_decompiler existent mais echouent (fp=0 bytes). Reparer
la lecture du programme (adresse/loc) puis la decompilation. PSO par shader.
### B3. Draws vertex-array (DRAW_ARRAYS) pour la 3D du monde
upload_vertices_from_rsx existe ; finir le mapping attribs/formats + MVP par draw.
### B4. Presentation / timing flip
vblank handler, double buffer, cadence.

---

## PHASE C — Pipeline d'assets
Edge zlib deja HLE (LFQueuePush). Une fois A debloque, verifier que le streaming
(.psarc / data/) decompresse bien vers VRAM/main. Le VFS monte deja l'arbo du jeu.

## PHASE D — Audio / Input gameplay / Save / Trophies / Polish (Guide Phase 10/12)
cellAudio (bridges presents), cellPad (fait), cellSaveData, cellTrophy.

---

## Chemin critique
A0 -> A1 -> A2 -> A3 -> A4 == premier FLIP avec contenu == le grand deblocage.
Ensuite B1/B2 rendent le visuel reel. Tout le reste suit.

## Reference indispensable
RPCS3 source (github.com/RPCS3/rpcs3), surtout Emu/Cell/Modules/cellSpurs* et
Emu/RSX/* . C'est l'implementation de reference de tous les sous-systemes manquants.

## Note d'honnetete
C'est un effort de l'ordre de plusieurs mois. Pour JOUER a UC3 sur PC maintenant,
RPCS3 le fait. Ce portage est un projet d'ingenierie au long cours.
