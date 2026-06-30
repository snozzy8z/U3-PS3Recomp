# Uncharted 3 (BCES01175) - Etat et feuille de route

Derniere mise a jour : 2026-06-28.

## Verdict actuel

MISE A JOUR 2026-06-28c : franchissement MAJEUR. Apres le garde-fou banks.txt
(borne de boucle absurde dans func_00767A18) qui a elimine l'OOM du pool scratch,
le jeu n'est PLUS en deadlock dur : il PROGRESSE par phases d'init lourdes
successives. Observe sur un boot de 150 s :
- pool de ~103 workers `DCLoader` cree puis termine (slot THREAD 14 reutilise),
- une phase `func_00038FBC` FINIE de ~22.5M iterations (~60 s, tid 0 ; c'est un
  dispatcher overlay a 0x012C4D10, pas un deadlock — elle se termine),
- emission de commandes GCM sur plusieurs contextes successifs (le contexte et le
  command buffer changent : 0x3074F040 puis 0x3074F000 -> le jeu avance),
- `cellGcmSetFlipMode` appele en continu, workers Save/Load + SPU fences.

Limites : (1) `cellGcmSetFlipCommand` JAMAIS appele (0x) -> le PREMIER FLIP n'est
pas atteint ; le jeu est encore en CHARGEMENT/SETUP, pas dans la boucle de rendu
en regime. (2) TRES LENT : chaque phase d'init prend des dizaines de secondes
(perf du code recompile + dispatch de taches), donc 150 s ne suffisent pas a
finir le setup. (3) INVISIBLE : pas de fenetre ni de backend RSX reel. Un backend
RSX null (drainer FIFO GET=PUT=REF) a ete ajoute dans `br_cellGcmInitBody`
(stubs.cpp) ; il draine le FIFO mais ne change pas la progression (le blocage
n'est pas GET mais la duree des phases de setup).

Ancien verdict : binaire vivant 60 s sans boucle ni flip. Nouveau : le moteur
tourne et progresse a travers l'init, mais lentement et sans encore atteindre le
premier flip ni presenter d'image.

MISE A JOUR 2026-06-28e (analyse pile multi-sample) : le thread principal n'est
PAS bloque dans une boucle garbage — il progresse dans de la LOGIQUE DE JEU
NORMALE. Chaine stable observee : func_0003F9D4 -> func_0064F288 -> func_0073BCE4
-> func_008C8464 -> func_00769550 -> func_00790320. Verifie : func_008C8464
preserve r31 correctement (sauve +0xB8, restaure avant blr) -> PAS de clobber ABI
ici (la violation r31 vue plus tot etait transitoire). Le sample dans
func_00790320 tombait dans l'EPILOGUE (loc_007904C4), et sa boucle ne fait que 16
iterations (r31<0x10) -> fonction normale, pas un spin. CONCLUSION : le "mur" est
la LENTEUR (perf du code recompile + init lourde), pas un deadlock. Le formatter
(func_00D79450 ~573M appels indirects, sur un worker probable) est le plus gros
puits de CPU et ralentit tout. Pistes les plus rentables : (1) trouver/borner la
boucle qui appelle le formatter 573M (liberer du CPU), (2) laisser tourner plus
longtemps pour voir si le 1er flip arrive (test 15 min en cours). Le jeu marche,
il est juste lent + invisible.

Le boot valide maintenant les budgets memoire et toutes les configurations PRX,
initialise FIOS, GCM, SPURS, les archives, les polices, les game data, les huit
ports audio et les peripheriques. La phase courante traite `banks.txt` (725
entrees) et cree des workers `DCLoader`; le thread principal est observe dans la
routine de hash de chaines `func_007199FC`. Cette phase n'est pas terminee apres
60 secondes et aucune image n'est encore presentee.

## PHASE 10 INPUT FAITE 2026-06-29p : cellPad polling bridge sur backend XInput

GAME_PORTING_GUIDE Phase 10 (Audio+Input), partie Input. libs/input/cellPad.c a deja
un backend XInput complet (mapping XInput->CELL_PAD_CTRL_*). Mais SEUL cellPadInit
etait bridge dans stubs.cpp -> le polling n'atteignait pas le jeu. Ajoute les bridges
(stubs.cpp) + enregistrements HLE pour : cellPadGetData(0x8B72CDA1),
cellPadGetInfo2(0xA703A51D), cellPadSetPortSetting(0x578E3C98),
cellPadGetCapabilityInfo(0xDBF4C59C), cellPadSetActDirect(0xF65544EE),
cellPadClearBuf(0x0D5F2C14), cellPadEnd(0x4D9B75D5). NIDs via nid_database.compute_nid
(matchent les NID firmware connus). Marshaling : appel dans un struct hote local
(HostPadData/HostPadInfo2) puis vm_write32/vm_write16 vers le guest (big-endian).
RESULTAT : [cellPad] Init(max_connect=2) appele+resolu. Le polling (cellPadGetData)
s'exercera des que le jeu est interactif (bloque par le SPU comme le rendu). Audio :
bridges cellAudio deja presents (br_cellAudioInit...). Phase 10 input = plomberie OK.

## PHASE 9 — CONFIRMATION FINALE 2026-06-30 : 0 texture peuplee -> converge sur Phase 11

Verifie exhaustivement (bind_texture log etendu, 74 textures) : AUCUNE texture peuplee
(loc=0 main: 21, loc=1 VRAM: 53, TOUTES a zero). Aucun raccourci (pas de font/UI
texture deja en main memory). Le contenu visuel (textures) depend ENTIEREMENT du
pipeline d'assets = SPU. Phase 9 (contenu texture visible) ne peut PAS se terminer
sans Phase 11 (SPU), qui est le gap documente (MODULE_STATUS: tasksets = no SPU exec).
La geometrie inline du menu rend (fallback PSO) ; les shaders se decompilent (fix fp
location) ; mais textures+geometrie3D = gate SPU. Phase 9 est faite au MAX possible
sans Phase 11. La doc-order force donc vers Phase 11 = le grand chantier.

## PHASE 9 STEP 5 (SHADERS) AVANCE 2026-06-30 : fix localisation fragment program

Suivi de l'ordre doc (Phase 9 avant 11). Step 5 = shader translation. BUG trouve &
corrige (meme classe que les textures) : rsx_commands.c masquait la LOCATION du
fragment program (fragment_program_addr = data & ~3). Les FP sont en VRAM (loc=1) ->
le backend lisait vm_base+offset (faux) -> rsx_fp_program_size=0 -> decompilation
ECHOUAIT TOTALEMENT. FIX (rsx_d3d12_backend.c get_or_create_shader) : fp_loc =
shader_program&3 ; fp_addr = (loc==1)? 0xC0000000+off : off. RESULTAT : fp lu (32o),
le FP + VP se DECOMPILENT en HLSL valide (FP: rsx_tex[0].Sample(tc0)*col0 ; VP:
pos+sorties). Plus aucun "Decompilation failed".
RESTE Step 5 : "Decompiled PSO creation failed 0x80070057" (E_INVALIDARG) car le PSO
decompile utilise la ROOT SIGNATURE BASIQUE (rsx_d3d12_backend.c:1404 rs=root_signature)
qui ne declare PAS t0/s0 (Texture2D[16]/Sampler[16] du FP). extended_root_sig est
declare (l.138) mais jamais cree. -> A FAIRE : creer une root sig avec CBV b0 + table
SRV t0-15 + sampler s0-15, l'utiliser pour les PSO decompiles. Le backend retombe
sinon sur le PSO fallback (couleur-vertex) -> la geometrie menu rend quand meme.
CONVERGENCE : meme avec le PSO texture, les textures sont VIDES (VRAM non peuplee ->
assets -> SPU). Donc Phase 9 (contenu texture visible) converge sur Phase 11 (SPU),
comme etabli. Phase 9 Step 5 = shaders decompiles OK ; binding texture = gate SPU.

## PHASE 9 TEXTURES = BLOQUE SUR LE SPU 2026-06-29s : la VRAM n'est jamais peuplee

Tentative texture upload (dernier maillon Phase 9). Diagnostic de l'adressage texture :
- bind_texture : loc=1 = CELL_GCM_LOCATION_LOCAL -> texture en VRAM RSX (base
  0xC0000000, cf cellGcmSys s_config.localAddress). EA = 0xC0000000 + tex->offset.
- Dump DXT1 (UC3_DUMP_TEX -> BMP) a vm_base+0xC0000000+offset : TOUT ZERO. La VRAM
  n'est jamais peuplee.
- edge-zlib (decompression LFQueuePush) = 0 appels ; les 10 bind_texture ont tous
  raw bytes = 0. => le PIPELINE D'ASSETS NE TOURNE PAS : aucune texture chargee.

CONCLUSION (honnete) : le texture upload GPU n'a RIEN a uploader (VRAM vide). La
cause = le streaming/decompression d'assets de UC3 depend du SPU (manager SPURS +
jobs) qui ne tourne pas. Donc :
  - Geometrie du menu : rend SANS le SPU (quads, cf 29q/29r). 
  - Textures du menu : NECESSITENT le SPU (pipeline d'assets). 
=> Phase 9 (textures) et Phase 11 (SPU) sont COUPLEES pour le visuel complet. Ecrire
le code D3D12 de texture maintenant serait inutile (pas de data). Phase 9 est faite
au maximum possible SANS SPU (geometrie visible). Le visuel complet (textures, et la
geometrie 3D du monde) passe par debloquer le SPU (Phase 11, env kernel SPURS @LS
0xBEC0). La boucle se referme : le SPU est le verrou central convergent.

## PHASE 9 GEOMETRIE VISIBLE 2026-06-29r : quads du menu affiches (gradient texcoord, debug)

Suite de 29q. Texcoord capture (attr1 -> inline_verts[4..5]) + d3d12_draw_inline rend
maintenant une COULEUR DEBUG derivee du texcoord (0.30+0.70*uv) -> les quads du menu
sont VISIBLES a l'ecran (layout/gradient) au lieu de noir. UC3_RSX_RAWCOL pour la
couleur vertex brute. Run 45s, pas de crash, 20+ draws/frame, backend D3D12 ready.
=> Premiere geometrie de jeu VISIBLE end-to-end (PPU->FIFO->rsx_commands->D3D12).
   bind_texture montre un atlas UI 4096x256 DXT1 (fmt 0xA6 = 0x86|0x20 = COMPRESSED_DXT1).
RESTE (vrai menu) : upload texture BC1 + PSO texture + sampler + SRV heap + le frag
shader echoue a decompiler (fp=0 bytes -> fallback). C'est le dernier maillon Phase 9
(texture upload), gros morceau D3D12 multi-iterations. La geometrie+texcoord sont prets.

## PHASE 9 GEOMETRIE INLINE FAITE 2026-06-29q : la geometrie du menu rend (immediate-mode)

DECOUVERTE (Phase 9 Step 3 "identify draw patterns", fait rigoureusement via dump
des blocs begin/end) : la geometrie du MENU/UI n'utilise PAS DRAW_ARRAYS — elle est
soumise en VERTEX INLINE immediate-mode entre BEGIN/END :
  BEGIN prim=8 (QUADS)
    0x1C20 (SET_VERTEX_DATA4F_M attr2) cnt=4   <- couleur/attr
    0x1888 (SET_VERTEX_DATA2F_M attr1) cnt=2   <- texcoord
    0x1880 (SET_VERTEX_DATA2F_M attr0) cnt=2   <- POSITION (+-1.0 = clip space)
  (x4 vertices = 1 quad) END
=> C'est pour ca que draws=0 : le parser ne gerait QUE les DRAW_ARRAYS explicites.
   ET c'est RENDABLE SANS LE SPU (le SPU sert a la geometrie 3D du monde, pas a l'UI).

IMPLEMENTE (Phase 9, multi-fichiers) :
- rsx_commands.h : rsx_state += inline_attr[16][4] scratch + inline_verts[16384][8]
  (pos.xyzw+col.rgba) + inline_vert_count ; callback rsx_backend.draw_inline ;
  defines NV4097_SET_VERTEX_DATA2F_M(0x1880)/4F_M(0x1C00).
- rsx_commands.c : capture SET_VERTEX_DATA2F_M/4F_M par attribut, commit un vertex
  a l'ecriture de l'attr0 (position = latch), emet draw_inline au END.
- rsx_d3d12_backend.c : d3d12_draw_inline = upload BasicVertex(pos+col), expansion
  QUADS(prim8)->triangle list (0,1,2,0,2,3), record draw TRIANGLELIST. MVP identite
  convient (verts deja en clip space).
- rsx_null_backend.c : draw_inline log.

RESULTAT (run 50s, pas de crash, D3D12) :
  [D3D12] draw_inline #0 prim=8 verts=4 pos0=(-1.000,1.000) col0=(0,0,0,1)  <- quad plein ecran
  [D3D12] draw_inline #1 prim=8 verts=4 pos0=(0.609,-0.444) ...             <- element UI
  20+ draws. La geometrie du menu est DESSINEE par le GPU.
=> Couleurs (0,0,0,1) = quads TEXTURES (couleur vertex noire module une texture) ;
   textures pas encore uploadees -> quads noirs. DERNIER maillon Phase 9 = TEXTURE
   UPLOAD (doc "What's NOT done: Texture upload"), apres quoi le MENU sera VISIBLE
   sans dependre du SPU. Phase 9 geometrie inline = OK.

## PHASE 9 ETAPE 2 FAITE 2026-06-29o : backend D3D12 reel actif (real GPU rendering)

Suivi du GAME_PORTING_GUIDE / RSX_GRAPHICS.md "Integration Guide" dans l'ordre :
  Step 1 Null backend ........ FAIT (fenetre GDI + clear)
  Step 2 D3D12 backend ....... FAIT (ce jalon)
  Step 3 Identify draw patterns / Step 4 vertex upload / Step 5 shader xlat .. bloques
         tant que draws=0 (la geometrie vient des jobs SPU -> Phase 11).

CABLE : stubs.cpp thread de presentation tente rsx_d3d12_backend_init (fallback null
si echec / UC3_NULL_RSX). CMake uc3 : ajout dxguid.lib (+ d3d12/dxgi) car le backend
utilise les GUID COM (IID_IDXGIFactory4 etc.) que le #pragma comment ne linke pas.

RESULTAT (run 50s, pas de crash) :
  [D3D12] Device created (feature level 11.0)
  [D3D12] Depth buffer created (1280x720 D24S8)
  [D3D12] Pipeline state created (triangle/line/point class)
  [D3D12] Vertex buffer created (112 KB)
  [D3D12] Backend ready: 1280x720
  [D3D12] set_render_target / set_blend / set_depth_stencil  (chaque frame)
  [D3D12] Shader cache miss: vp=3 instrs, hash=0xC99F7D41  <- voit les shaders du jeu
=> Fenetre D3D12 GPU reelle, consomme l'etat RSX + shaders. draws=0 (geometrie SPU).
Le backend D3D12 est le defaut maintenant (UC3_NULL_RSX pour forcer le null GDI,
UC3_NO_WINDOW pour desactiver).

## PHASE A2 RACINE AFFINEE 2026-06-30 : il manque le KERNEL/POLICY en LS 0x0-0x3000 (pas 0xBEC0)

CORRECTION via dump RPCS3 (user) du SPU qui execute la taskset (0xBEC0=0x1007000A =
NOTRE valeur exacte) :
  [0xBEC0] 10 07 00 0a -> 'hbra 0xbee8,0x3800'  (du CODE, hint-branch, pas une table)
  [0x2700] 01 a0 0d 96 -> 'rdch r22,MFC_RdAtomicStat'  (CODE)
  [0xC400] d6 00 eb a5 -> 'fnms r48,r87,r3,r37'        (CODE)
=> NOTRE LS@0xBEC0 EST CORRECT (identique au reel). Le probleme n'etait PAS la valeur
de 0xBEC0. Le SPU reel a du CODE aux adresses BASSES (0x2700, 0xC400, 0xBEC0...) : le
KERNEL SPURS + POLICY MODULE + task sont charges et SE SUPERPOSENT en LS. Notre run ne
charge QUE la task (spu_0007 @0x3000+) et laisse LS 0x0-0x3000 (et les zones overlay)
VIDES. Le dispatch (func_00009560) lit une adresse basse (hand-sim ~LS@0xE0, zone
kernel) -> VIDE chez nous -> cible garbage. (Note: mes hypotheses kernel ctx@0x100 /
taskset ctx@0x2700 comme DATA etaient fausses : a 0x2700 il y a du CODE kernel, pas le
SpursTasksetContext data - la structure RPCS3 decrit un layout DATA mais le binaire
reel met du code la ; le layout exact depend de l'image chargee).

VRAI DEBLOCAGE A2 : charger le CODE+DATA du KERNEL SPURS + POLICY MODULE en LS
0x0-0x3000 (et zones overlay) comme le fait RPCS3. Le policy module pm=0x010D8C00 est
extractible (EBOOT) ; le KERNEL SPURS est firmware. 
=> Voie la plus directe : DUMP COMPLET du LS (256KB, ou au moins 0x0-0x3000) du SPU ou
0xBEC0=0x1007000A, depuis RPCS3, et le charger dans spu_context.ls AVANT de lancer la
task. Caveat : les pointeurs DATA dans le LS sont run-specific (adresses heap RPCS3) ->
les DMA iraient vers de mauvaises adresses ; le CODE (kernel/policy) est portable. Donc
un dump LS debloque l'EXECUTION pour analyse mais pas forcement les DMA. Voie propre
finale = HLE du kernel SPURS (gros). 

## PHASE A2 RACINE TRACEE 2026-06-29z : LS@0xBEC0 = table dispatch runtime (defaut ELF insuffisant)

Trace statique COMPLETE de la chaine du DMA fautif :
- func_0000A29C : EA du DMA = gpr[82] doubleword prefere.
- func_0000A248 (ligne 8832) : gpr[82] = gpr[3] (l'arg r3 de la fonction).
- A248/A294/A29C sont atteints via le DISPATCH en func_00009560, qui fait gpr[3]=il(0)
  PUIS calcule la cible : gpr[7]=ls_read128(0xBEC0) ; gpr[6]=rotqbyi(gpr[7],8) ;
  gpr[2]=gpr[6].w0+196 ; gpr[5]=ls_read128(gpr[2]) ; gpr[4]=rotqby(gpr[5],gpr[2]) ;
  pc=gpr[4].
- LS@0xBEC0 = valeur DEFAUT de l'ELF (10 07 00 0A 00 08 00 61 00 08 00 21 00 09 00 A3).
  Hand-sim : gpr[2]=0x800E5 (hors LS -> masque 0xE5) -> ls_read128(~0) -> cible GARBAGE
  -> on tombe dans une mauvaise fonction avec gpr[3]=0 -> gpr[82]=0 -> DMA ea=0.

RACINE DEFINITIVE (boucle bouclee avec 29n) : LS@0xBEC0 est la TABLE DE DISPATCH
SPURS. L'ELF n'a qu'un DEFAUT ; le POLICY MODULE / KERNEL SPURS calcule sa vraie
valeur RUNTIME (les paires id/offset y deviennent des pointeurs/cibles valides).
Sans ce runtime, le dispatch part en garbage.

=> 2 voies pour finir A2 :
 (1) Obtenir le LS@0xBEC0 RUNTIME reel : dump du Local Store du SPU executant la
     taskset Uncharted dans RPCS3 (debugger, au moment ou la tache tourne), region
     ~0xBE00-0xC400. Le charger -> le dispatch trouve la vraie cible. (Valeur
     run-specific mais debloque l'execution pour analyse.)
 (2) HLE de la computation que le policy module fait pour remplir 0xBEC0 (RE du
     policy module / ref RPCS3 cellSpursSpu taskset dispatch). Plus lourd.
La voie (1) est la plus directe maintenant que l'utilisateur a RPCS3 + sait dumper.

## PHASE A2 LOCALISATION 2026-06-29y : le DMA fautif = func_0000A29C, EA=gpr[82] (=0)

Analyse statique du code lifte : le 1er DMA (GET ea=0 size=128 -> LS 0x36D00) est
emis par spu_func_0000A29C (spu_gen/u3/spu_0007.c ~ligne 8858) :
  gpr[10] = 0x36D00 + gpr[126]            -> MFC_LSA
  EAH = gpr[82].word0 ; EAL = shlqbyi(gpr[82],4).word0 = gpr[82].word1
  -> EA = gpr[82] doubleword prefere ; size=128 ; GET
Donc l'EA = pointeur dans gpr[82]. 128 octets = en-tete CellSpursTaskset
(running@0/ready@0x10/.../task_info@0x80). gpr[82] DEVRAIT etre le pointeur TASKSET
(0x3115D080). A l'entry 0x3050, gpr[82]=ori(gpr[4],0). Mais a func_0000A29C il vaut 0
-> clobbere ou mal propage dans la chaine entry->9558->9560->dispatch->...->A29C
(r80-127 sont callee-saved ; une fonction le perd ou le recharge depuis une struct
vide). 
=> Resolution = TRACER le flux de gpr[82] : re-lift spu_0007 avec spu_lifter --trace,
run, suivre gpr[82] (r82) de l'entry jusqu'a 0xA29C pour voir ou il devient 0 / d'ou
il devrait etre recharge (probablement le pointeur taskset depuis LS@0x27B8 taskset
ctx, ou r4._u64[? ] lane). C'est l'etape suivante precise et bornee. (Soupcon: lane
order r4 _u64 vs _u32, ou le taskset ptr doit etre injecte ailleurs.)

## PHASE A2 PROFONDE 2026-06-29x : spursTasksetStartTask reproduit ; reste 1 pointeur 0 (besoin --trace)

Recupere l'impl exacte de spursTasksetStartTask (RPCS3 cellSpursSpu.cpp) et reproduit
TOUT son setup : r2=0, r3=task args (16o), r4._u64[0]=spurs/._u64[1]=taskset args,
r5..127=0, pc=ELF entry ; + zero du task area (spu_context_init le fait) ; + contexts
LS (kernel @0x100, taskset @0x2700) + wklInfo @0x3FFE0 + tsargs fallback = wklInfo.arg
(0x3077F380). NOTE: le policy module pm=0x010D8C00 N'est PAS auto-contenu (il appelle
le kernel SPURS firmware absent) -> le HLE C++ (a la RPCS3) est la bonne voie, PAS le
lift du binaire policy.

MUR RESIDUEL : malgre tout ca, le 1er DMA de la tache reste GET ea=0 size=128 (lsa
0x36D00) -> un pointeur lu vaut 0, d'une source NON identifiee parmi les structures
posees. Deviner structure-par-structure (cycle build ~90s) a atteint sa limite.
=> PROCHAIN OUTIL (doc SPU_LIFTER.md sec.3 Tracing) : re-lifter spu_0007 avec
spu_lifter --trace, lancer, et trouver l'instruction qui ecrit 0 dans MFC_EAL juste
avant le 1er GET (remonter au registre/lecture LS source). Cela pinpointe la derniere
valeur manquante. Alternative : capturer le LS reel d'un SPU dans RPCS3 (debugger) au
moment ou la tache demarre, et diff avec notre LS.
Etat build : defaut OK ; chemin gate UC3_SPU_TASK = abort propre (garde 3M ops), pas
de crash. Tout le setup SPURS est dans stubs.cpp uc3_spu_task_run + br_cellSpurs*.

## PHASE A2 DEMARREE 2026-06-29w : workload info posee ; chemin = executer le policy module

AddWorkload peuple CellSpurs.wklInfo1[wid] @spurs+0xB00 (addr=pm=0x010D8C00,
arg=0x3077F380 = base taskset/fence reelle, vue aussi dans [spu-fence]). Le harnais
copie 20B vers LS@0x3FFE0. MAIS le 1er DMA de la tache reste ea=0 : le task entry
(0x3050) lit un pointeur depuis LS@0xC400/0xBEC0 que le POLICY MODULE pose AVANT de
demarrer la tache - or on saute le policy module (jump direct a 0x3050).
=> CHEMIN A2 (doc ROADMAP_LONGTERM section A2) : LIFTER+EXECUTER le policy module
pm=0x010D8C00 (code SPU brut, pas un ELF) comme workload entry, qui fait le setup LS
complet + demarre la tache correctement. Verifier d'abord si spu_0007 (deja lifte)
contient l'entry kernel/workload (0xA00) - c'est l'image runner, il l'a peut-etre.
Build stable (defaut OK ; chemin gate UC3_SPU_TASK = abort propre via garde, pas de crash).

## PHASE A1 TERMINEE 2026-06-29v : environnement SPURS pose + garde anti-emballement -> A2

A1 (environnement de tache SPURS) = FAIT et STABLE :
- LS : kernel context @0x100 (spurs@0x1C0), taskset context @0x2700 (taskset ptr
  @0x27B8, spuNum/dmaTagId/taskId). 
- Main-memory CellSpursTaskset peuple dans br_cellSpursCreateTask (spurs@0x60,
  enabled/ready bits, task_info[0]@0x80 : args/elf/ctx_save/ls_pattern). Layouts =
  RPCS3 cellSpurs.h / cellSpursSpu.cpp (cf ROADMAP_LONGTERM.md A0).
- Registres ABI : SP=0x3FFB0, gpr[3]=args, gpr[4]=spurs/taskset args.
RESULTAT : 0 branche inconnue, le manager s'execute LONGUEMENT (100k+ DMA) avec
l'environnement.

FRONTIERE A2 (job queue) identifiee : sans jobs reels dans la queue, le manager
tombe dans une BOUCLE DEGENEREE (PUT lsa==ea size=2, 3M+ ops) -> debordement de pile
hote. AJOUTE une GARDE anti-emballement (spu_channels.c : setjmp/longjmp via
spu_abort_arm + compteur d'ops, seuil 3M ; harnais fait setjmp autour de l'entry).
=> le chemin gate UC3_SPU_TASK ne CRASHE PLUS (abort propre, exit 124) ; build par
defaut intact (345 events rendu, exit 124).

PROCHAIN = A2 : fournir le travail. Lire la struct CellSpurs reelle (0x21AF3E80) dans
le dump PS3 (base +0x2BF0000) pour comprendre les workload info / job queues, et soit
peupler une vraie queue de jobs, soit detecter "pas de job" pour sortir le manager
proprement. Puis charger+executer le code de job (spu_0000/0001 = geometrie). Detail :
ROADMAP_LONGTERM.md section A2.

## PHASE A1 SUITE 2026-06-29u : kernel ctx pose, reste taskset ctx @0x2700 (structures interlock)

Apres 29t : applique aussi
- gpr[4] spurs = adresse SPURS reelle (0x21AF3E80) recuperee de g_spurs_states (le
  taskset main-memory etant a 0). 
- SpursKernelContext @LS 0x100 (layout RPCS3 cellSpurs.h) : spurs ptr @0x1C0 (EA 32
  bits -> 0x1C4), spuNum@0x1C8, dmaTagId@0x1CC, wklCurrentId@0x1DC.
Log DMA SPU active (spu_channels.c #define SPU_DMA_LOG + g_spu_dma_log).

OBSERVE (trace DMA, gate UC3_SPU_TASK) : le manager fait
  GET lsa=0x36D00 ea=0x00000000 size=128   (x2)  <- lit une struct via pointeur NUL
  GET lsa=0x0C480 ea=0x00003300 size=64 (+0x40 incrementiel, ~2.3KB) <- copie table
    depuis base ~0 + offset
Ces EA (~0) ne derivent NI de spurs(0x21AF3E80) NI du kernel ctx -> le pointeur vient
d'une struct encore vide = le TASKSET CONTEXT @LS 0x2700 (SpursTasksetContext) que la
tache lit pour ses pointeurs. Crash (segfault) quand un pointeur calcule depuis ce
garbage est deref.

DIAGNOSTIC : HLE du kernel SPURS = peupler des structures qui s'enchainent. Faites :
kernel ctx @0x100. RESTE : (1) SpursTasksetContext @LS 0x2700 (besoin du layout RPCS3 -
SpursTasksetContext dans cellSpursSpu.cpp/cellSpurs.h) + (2) la struct main-memory
CellSpursTaskset (le taskset que CreateTask recoit en r3 - verifier qu'il est peuple :
notre CreateTaskset2 ecrit spurs@+0x60/args@+0x68 mais le jeu utilise peut-etre
CreateTaskset (stub) -> taskset vide) + (3) workload info @LS 0x3FFE0 (depuis
CellSpurs.wklInfo1 @main+0xB00, entree 0x20o : addr@0/arg@8/size@0x10/uniqueId@0x14).
NOTE: la tache a ete lancee DIRECTEMENT a l'entry 0x3050 en court-circuitant le
kernel+policy module ; l'alternative propre serait d'executer le vrai flux
(kernel entry -> dispatch workload @0xA00 -> start task) mais ca demande le policy
module SPU (pm=0x010D8C00, non extrait). Le HLE des structures est la voie choisie.
Offsets CellSpurs connus (RPCS3) : wklReadyCount1@0x00, wklState1@0x80, wklInfo1@0xB00
(0x20/entree), wklInfo2@0x1000. Reference dump PS3 (base +0x2BF0000) pour valeurs reelles.

## PHASE A1 PROGRES 2026-06-29t : le manager SPU S'EXECUTE (ABI SPURS appliquee)

A0 fait (ABI via RPCS3 cellSpursSpu.cpp, cf ROADMAP_LONGTERM.md). Applique dans le
harnais (stubs.cpp uc3_spu_task_run) :
- gpr[1] = SP = 0x3FFB0 (CRITIQUE : avant gpr[1]=0 -> le prologue ecrivait en
  LS[0x3FFE0] et SP devenait garbage -> 61667 branches folles). 
- gpr[3]=task args, gpr[4]=spurs/taskset args (lanes _u32 = mots BE).
RESULTAT : branches folles 61667 -> 0. Le SP fix a SUPPRIME la corruption.
- Ajoute 0x4098/0x40A0 (cibles jump-table mid-fonction) comme fonctions et re-lifte
  spu_0007 (--functions, 538 fns) -> spu_indirect_branch les resout. unknown=0.
=> Le manager spu_0007 DISPATCHE et EXECUTE maintenant le code reel (plus de
divergence). NOUVEAU mur : SEGFAULT (exit 139) plus loin, car la structure SPURS est
incomplete : spurs=0, tsargs=0 (le taskset main-memory n'est pas peuple par nos stubs
au moment du CreateTask) -> le manager lit des pointeurs nuls -> DMA invalide.

PROCHAIN (A1 suite) : peupler l'etat SPURS pour que le manager ait des pointeurs
valides : (a) main-memory : s'assurer que le taskset (r3 de CreateTask) a spurs@+0x60
et args@+0x68 corrects (verifier l'ordre CreateTaskset2 vs CreateTask) ; (b) LS :
poser kernel context @0x100, taskset context @0x2700, workload info @0x3FFE0 (20B,
copie depuis la main-memory SpursWorkloadInfo). Layout dans ROADMAP_LONGTERM.md A0.
Une fois sans crash -> A2 (pull de job) -> A3 (geometrie).

## POINT 1 CAUSE-RACINE 2026-06-29n : divergence = table de dispatch SPURS manquante en LS@0xBEC0

Analyse statique du task body spu_func_00009560 (le main de la tache manager) :
  gpr[7] = spu_ls_read128(0xBEC0)            ; lit la table de dispatch (LS)
  gpr[6] = rotqbyi(gpr[7], 8)                ; extrait un champ
  gpr[2] = gpr[6] + 196
  gpr[5] = spu_ls_read128(gpr[2])            ; lit une entree de jump-table
  gpr[4] = rotqby(gpr[5], gpr[2])
  ctx->pc = gpr[4]._u32[0]; spu_indirect_branch(ctx)   ; SAUT calcule depuis le LS

LS@0xBEC0 est VIDE dans notre run (le kernel SPURS aurait du y DMA/placer la table de
dispatch des taches + le contexte). gpr[7]=0 -> gpr[2]=196 -> lit LS@196 (zero) ->
gpr[4]=garbage -> branche vers 0xE0FF/0x1C0FF/0x0. DONC la divergence est 100% due a
l'ENVIRONNEMENT KERNEL SPURS ABSENT (la structure en LS@0xBEC0), PAS au lifting (qui
execute fidelement les instructions).

CE QU'IL FAUT (frontiere de recherche) : reproduire le layout LS du kernel SPURS pour
une tache — au minimum la table de dispatch en 0xBEC0 (+ le contexte taskset que le
PPU passe via ctx_save=0x3115C080 et cellSpursCreateTaskset2). Cela necessite l'ABI LS
du kernel SPURS (layout des structures SPURS en local store), qui N'EST PAS dans les
docs du projet (les docs declarent l'exec SPU comme "gap connu"). Reference necessaire :
source RPCS3 (Emu/Cell/SPURS*) ou la doc ABI SPURS du SDK Sony. C'est la le coeur
RPCS3-level ; le harnais (UC3_SPU_TASK) + les 8 lifts sont prets a l'accueillir des que
le layout 0xBEC0 est connu/rempli.

## POINT 1 CARTOGRAPHIE 2026-06-29m : classification des 8 images SPU (doc Phase 11 "identify task type")

Densite float (fa/fm/fma) vs DMA (wrch) par image :
  spu_0007 : 0/0/0 float, 127 wrch, 132 shufb -> MANAGER SPURS / orchestrateur DMA
             (= la tache persistante unique creee par cellSpursCreateTask). Diverge
             standalone car il lui faut ses queues + l'env kernel.
  spu_0000 : 399/828/506 float, 871 wrch -> JOB GEOMETRIE/VERTEX Edge (math + DMA out)
  spu_0001 : 373/570/317 float, 642 wrch -> JOB GEOMETRIE/VERTEX Edge
  spu_0002 : 90/189/115  + 39 cflts -> job math
  spu_0003 : 161/350/299 + 97 cflts -> job math (anim/physique ?)
  spu_0004 : 271/366/208 + 132 cflts -> job math
  spu_0005 : 231/277/168 + 127 cflts -> job math
  spu_0006 : minimal -> utilitaire

ARCHITECTURE : le PPU soumet des descripteurs de job -> le MANAGER (spu_0007) les
tire et DMA le code de job (spu_0000/0001 = geometrie) dans le LS pour l'executer.
La GEOMETRIE MANQUANTE du FIFO (draws=0) vient de spu_0000/0001 non executes.

CHEMIN DOC-ALIGNE (Phase 11 HLE-preferred) pour la geometrie visible : NE PAS tenter
de faire tourner le manager standalone (diverge). Plutot intercepter la SOUMISSION de
job cote PPU (ou la queue du manager) et executer DIRECTEMENT la fonction SPU liftee
du job geometrie (spu_0000/0001) avec l'input du job -> son DMA ecrit vertices+DRAWs
dans les segments FIFO reserves -> le walker FIFO (deja cable) les envoie au backend.
SOUS-PAS suivant = trouver ou le PPU soumet les jobs geometrie (Edge job submission /
la/les queue(s) du manager au-dela du LFQueuePush zlib deja HLE) et y brancher le
dispatch vers la fonction liftee. C'est de la recherche PPU-side + wiring, substantiel.

## POINT 1 AVANCE 2026-06-29l : harnais d'execution SPU construit et fonctionnel (diverge sur env SPURS)

CABLE (dans le build) :
- spu_gen/u3/spu_0007.c (lift de l'image-tache, copie 2-niveaux-sous-racine pour que
  ses includes ../../runtime/... resolvent ; spu_recomp.h = copie de spu_0007.h pour
  son #include interne). Ajoute a projects/uncharted3/CMakeLists.txt (SPU_LIFTED_SOURCES).
  Compile en C avec cl (warnings C4190 sur retour u128 en liaison C = inoffensifs).
- Harnais dans stubs.cpp (uc3_spu_task_run + hook dans br_cellSpursCreateTask, gate
  UC3_SPU_TASK) : parse l'ELF SPU **ELF32 big-endian** (EI_CLASS=1 ! pas 64), charge
  les PT_LOAD dans un spu_context.ls 256KB, met l'arg 128-bit (r9->4 mots) dans gpr[3],
  spu_recomp_register(), pc=entry, lance spu_func_00003050 sur un thread hote. Le DMA
  des fonctions liftees ecrit dans vm_base (memoire principale partagee) automatiquement.
- Cap du flood [SPU] indirect branch unknown a 32 lignes (runtime/spu/spu_channels.c).

RESULTAT (UC3_SPU_TASK=1) :
- ELF charge OK : 2 PT_LOAD, entry=0x03050.
- spu_func_00003050 = PROLOGUE de tache SPURS : configure registres, lit LS@0xC400,
  appelle spu_func_00003010, puis le task main spu_func_00009558(r3=arg...), puis
  spu_func_0000B0A0, puis STOP. S'execute donc reellement.
- arg passe = [0, 0x011D27D0(=TOC PPU!), 0, 0x0FEFEC40] — l'arg contient des pointeurs PPU.
- BLOCAGE : profond dans le task main, ~61k branches indirectes vers des cibles non
  resolues. 11 cibles uniques : un MELANGE de plausibles (.text: 0x3438,0x34A8,0x4098,
  0x40A0,0x6680,0xBFC0) ET de GARBAGE (0x00000,0x000B4,0x000E0,0x0E0FF,0x1C0FF).

DIAGNOSTIC : le garbage prouve une DIVERGENCE d'execution — la tache lit des zones LS
que le KERNEL SPURS aurait du initialiser (bloc contexte taskset, save area
ctx_save=0x3115C080, zone de comm kernel) avant de jumper a l'entry. Exécuter la tache
STANDALONE (sans HLE du kernel SPURS) -> donnees LS manquantes -> cibles calculees
invalides. (0x4098 est aussi au MILIEU de spu_func_00004090 = cible de jump-table que
le detecteur n'a pas promue, mais c'est secondaire vs la divergence.)

=> PROCHAIN SOUS-PAS (substantiel, RPCS3-level) : reproduire l'environnement de tache
SPURS dans le LS avant de lancer l'entry — soit (a) HLE du protocole taskset (poser le
contexte/save-area aux bonnes adresses LS comme le kernel), soit (b) lifter + executer
aussi le policy module SPURS (pm=0x010D8C00) pour qu'il pose l'env et appelle la tache.
La VRAIE difficulte du point 1 est la, pas le lifting (qui marche). Harnais pret a
iterer : UC3_SPU_TASK=1.

## POINT 1 DEMARRE 2026-06-29k : SPU lifte + cible d'integration identifiee (SPURS Task)

FONDATION (faite) :
- Les 8 images SPU extraites sont LIFTEES via `tools/spu_lifter.py --auto-functions`
  dans projects/uncharted3/spu_lifted/spu_000N/ : 7414 fonctions au total, couverture
  70-87%. spu_0006 COMPILE proprement avec cl (cl /c /std:c11 /I spu_lifted/spu_0006
  /I runtime/spu /I include). Foundation validee.

DISPATCH SPU DE UC3 (analyse) :
- UC3 utilise SPURS EXCLUSIVEMENT (pas de raw sys_spu_thread). cellSpurs* bridges
  partiels dans stubs.cpp. Workloads via cellSpursAddWorkload (pm=0x010D8C00 = policy
  module SDK partage).
- 2 chemins de travail SPU :
  (a) cellSpursLFQueuePush -> DEJA HLE : fallback Edge zlib (stbi_zlib_decode) qui
      inflate src->dst + signale event flag / decremente counter. Les jobs de
      DECOMPRESSION marchent deja cote PPU.
  (b) cellSpursCreateTask -> PUR STUB (ecrit 0, retourne OK, n'execute rien). C'est
      le chemin des tACHES SPU persistantes (geometrie Edge, etc.) = la geometrie
      manquante du FIFO.

CIBLE PRECISE (confirmee runtime) :
- cellSpursCreateTask appele avec elf=0x010CF700, ctx_save=0x3115C080, ctx_size=0xC00,
  pattern=0x00E3C6B8, arg=0x0FEFEBA8. UNE seule CreateTask/60s = worker PERSISTANT.
- elf guest 0x010CF700 -> file 0x10BF700 = ELF SPU (EM_SPU=23, entry=0x3050). Les
  images extraites etaient nommees par OFFSET FICHIER (delta va=file+0x10000), donc
  spu_0007_at_010BF700.elf EST cette image-tache, DEJA LIFTEE, et spu_func_00003050
  (l'entry) existe.

WIRING RESTANT (prochain gros sous-pas) : executer spu_0007 comme tache SPU
persistante. Necessite : (1) spu_context + LS 256KB ; (2) charger les PT_LOAD de
l'ELF dans le LS a leur p_vaddr ; (3) enregistrer un mfc_engine (DMA) + channels
(runtime/spu/spu_channels.c les fournit) ; (4) ABI tache SPURS : args/taskset en
registres+LS comme le ferait le kernel SPURS ; (5) lancer spu_func_00003050(ctx) sur
un thread hote, gerer DMA/channels/branches indirectes (spu_indirect_branch +
spu_recomp_register). DIFFICULTE : la tache attend l'environnement LS pose par le
kernel SPURS (elle lit taskset/workload via DMA) -> peut necessiter de HLE le
protocole taskset, pas juste lancer l'entry. C'est le coeur RPCS3-level de l'effort.

## JALON 2026-06-29i : PREMIERE SORTIE VISIBLE (fenetre + backend RSX consommant le FIFO)

Cable dans stubs.cpp (drainer FIFO) :
- thread de presentation : rsx_null_backend_init(1280,720) cree une fenetre Win32
  GDI et s'enregistre comme backend (auto rsx_set_backend), pump des messages.
- le drainer decode les en-tetes NV4097 du FIFO consomme et route chaque
  (method,data) vers rsx_process_method(&g_rsx_state,...) -> backend (clears/etat).
- desactivable via UC3_NO_WINDOW ; histogramme via UC3_FIFO_TRACK.

RESULTAT (run 60s, pas de crash) :
  [RSX null] Window created: 1280x720
  [RSX null] set_render_target(format=0x5000000, 720x0)  (x chaque frame)
  [RSX null] set_viewport(0,0 0x0)
=> Le jeu boote -> menu -> rend -> PRESENTE dans une fenetre. Pipeline end-to-end
prouve visuellement (jalon Phase 9 etape 1 "Displays RSX clear color").

VERIF 2026-06-29j (point 2) : le parsing surface/viewport N'A PAS DE BUG. Probe des
donnees reelles (UC3_FIFO_TRACK) :
  0x0200 SURFACE_FORMAT count=8 data0=0x05000000 (bloc incremental 0x200-0x21C,
         gere via method+i*4)
  0x0300/0x0304 VIEWPORT data=0  -> viewport REELLEMENT a 0 a ce stade (frames de
         setup, geometrie pas encore dessinee)
  0x1D94 CLEAR_SURFACE flags=0xF0 ; color_clear_value=0 -> CLEAR EN NOIR
  0x1D0 SET_COLOR_CLEAR_VALUE jamais mis non-nul.
=> La fenetre affiche fidelement ce que le jeu rend : clear noir, sans geometrie.
Les valeurs "degenerees" sont le contenu reel du FIFO pre-geometrie, pas un bug.
Le SEUL vrai blocage pour du contenu visible = les jobs SPU (geometrie). Point 2
clos. Probes laisses en place (gated UC3_FIFO_TRACK, supprimables).

RESTE pour un menu VISIBLE complet :
1. GEOMETRIE : draws absents (les vertices/DRAW viennent des jobs SPU Edge non
   executes) -> Phase 11 (HLE par type de tache, approche preferee doc).
2. Dimensions surface/viewport degenerees (720x0, viewport 0x0) : parsing des
   methodes multi-mot (surface clip / viewport H|W) a affiner dans le walker ou
   utiliser rsx_process_command_buffer.
3. Backend D3D12 pour vrai rendu GPU (au lieu du clear-color GDI).
4. cellGcmSetFlipCommand n'est pas appele par notre bridge (0x) : le flip passe
   peut-etre par un autre chemin (label FIFO) -> verifier la cadence de present.

## RECONCILIATION 2026-06-29h : le FIFO RSX N'EST PAS VIDE (squelette present, draws SPU absents)

Phase 9 "state tracking" (doc GAME_PORTING_GUIDE) : instrumentation du drainer FIFO
(stubs.cpp, UC3_FIFO_TRACK=1) qui decode les en-tetes NV4097 consommes au lieu de
les jeter. Resultat sur ~75s :
  headers=83003 methods=150035 begin_end=1596 draws=0 clears=399 jumps=799
Histogramme top methodes : 0x0200-0x03B0 (constantes transform/etat, ~1197 each),
0x1880/0x1888 (pointeurs attributs vertex), 0x1C20 (texture), 0x1D94 (clear),
0x1808 (begin/end). MAIS draws=0 sur TOUS les draws (0x1814/0x1818/0x1820/0x1824).

INTERPRETATION DECISIVE : le PPU emet deja le SQUELETTE FIFO complet (clears + etat
+ begin/end + ~400 jumps/frame vers des segments reserves), mais AUCUNE commande
DRAW ni donnee de vertex. C'est la signature du pipeline geometrie EDGE de Naughty
Dog : le PPU pose le squelette avec des trous (jumps vers segments reserves), et les
JOBS SPU remplissent les DRAWs + vertices dans ces segments a l'execution. SPU
absent => squelette present, geometrie absente.

=> Corrige la conclusion "FIFO vide". Un backend qui consomme ce FIFO afficherait
DEJA les CLEARS (couleur de fond du menu) + l'etat, sans geometrie. C'est exactement
le but du null backend de la doc ("Displays RSX clear color"). Le drainer custom de
ce projet ne feed PAS libs/video (il fait juste GET=PUT) -> rien n'est presente.

PROCHAINE ETAPE doc-prescrite (Phase 9 strat. 1+2) : cabler le FIFO sur
rsx_commands.c + un backend (null GDI puis D3D12) pour obtenir la PREMIERE sortie
visible (fenetre + couleur de clear du menu), prouvant le pipeline end-to-end SANS
attendre le SPU. La geometrie viendra ensuite avec l'execution SPU (Phase 11, HLE
par type de tache = approche preferee de la doc, PAS un interpreteur generique).

## DECOUVERTE POSITIVE 2026-06-29e : le jeu ENUMERE les vrais niveaux (phase MENU)

Sonde [worldlookup] sur func_008BBDB8 (match 'worl') : les lookups sont des VRAIS
niveaux d'Uncharted 3 — 'world-london', 'world-underground', 'world-syria' (lr
identique 0x20FA6930 = une boucle d'enumeration). Donc le registre DC/SID contient
les VRAIS niveaux du jeu, et le jeu ENUMERE les niveaux (il construit le menu de
selection de chapitres). 'world-test' etait un echec NON-FATAL ; le jeu a continue
jusqu'a la phase MENU.

=> Le verrou n'est PAS le contenu (les vrais niveaux sont la, le registre marche).
Le jeu est a la phase MENU, rend des frames (341 SetFlipMode) mais ne FLIPPE
jamais (SetFlipCommand=0) -> menu invisible. Le verrou est le FLIP/AFFICHAGE : la
boucle de rendu attend une condition (frame prete / vblank / event) pour flipper,
qui n'est jamais remplie (cf. func_00038FBC idle, flag 0x011DFDA8 jamais ecrit).

NOUVELLE CIBLE : faire arriver le 1er FLIP. Soit en satisfaisant la condition que
le jeu attend avant de flipper (l'event/vblank/flag), soit en implementant le
backend RSX + le signal de flip/vblank. Le jeu est BEAUCOUP plus loin que pense :
il est au menu, avec les vrais niveaux charges, en train de rendre. Il manque la
presentation (flip + backend).

CONFIRMATION 2026-06-29f : le blocage du scheduler = l'EXECUTION SPU/SPURS. Le log
montre cellSpursInitializeWithAttribute2(nSpus=6) puis ~20
cellSpursAddWorkloadWithAttribute (wid 3-23, programmes SPU a pm=0x010D8C00 etc.).
Ces WORKLOADS SPU sont enregistres mais JAMAIS EXECUTES (aucun interpreteur/recomp
SPU ne tourne les programmes). Le scheduler de jobs PPU (func_00038FBC) attend la
completion de ces jobs SPURS -> idle infini (obj+0x8C jamais != -1, flag jamais
ecrit) -> pas de flip -> menu invisible. C'est exactement la "limite connue"
(taches SPU non executees), et c'est LE verrou du 1er flip.

VERROU FINAL (2 sous-systemes majeurs, multi-mois) pour un menu visible :
  1. EXECUTION SPU/SPURS : faire tourner les programmes SPU des workloads (un
     interpreteur/recompileur SPU + le kernel SPURS qui ordonnance les taches),
     pour que les jobs se completent et que le PPU scheduler avance jusqu'au flip.
     C'est la cause directe de l'idle actuel.
  2. BACKEND RSX (D3D12) : pour presenter l'image une fois le flip emis.
Le runtime a deja un lifter SPU (runtime/spu/) et un fallback Edge DEFLATE ; il
faudrait brancher l'execution des workloads SPURS reels (les programmes a
0x010D8C00...) sur ce lifter. C'est l'axe le plus impactant restant.

CARTE DEFINITIVE DES ARTEFACTS SPU (2026-06-29g) :
- 8 images SPU de JOB du jeu DEJA EXTRAITES dans spu_programs/ :
  spu_0000@01001500 (140K), 0001@01023800 (167K), 0002@0104C700 (82K),
  0003@01060A80 (135K), 0004@01081B80 (117K), 0005@0109E800 (106K),
  0006@010B8800 (28K), 0007@010BF700 (38K, finit a 0x010C8BA0).
  => ce sont les VRAIS programmes de job (geometrie, generation FIFO RSX, etc.).
- pm=0x010D8C00 (policy module partage par wid 3-22) et 0x0100AF80 (wid 23) :
  AU-DELA de la derniere image -> ce sont les KERNELS SDK SPURS (taskset/jobqueue
  policy modules), HLE-ables (comme RPCS3 qui HLE le kernel SPURS).
- Un seul disasm fait : spu_disasm/spu_0007.json (536 instr).

PLAN CONCRET pour faire tourner les jobs (roadmap etape 5, large mais borne) :
  (a) HLE du kernel SPURS (policy modules) cote PPU : ordonnancer les workloads/
      tasks sans executer le code du policy module SPU.
  (b) Lifter les 8 ELF de job en C++ via tools/spu_lifter.py (deja teste sur
      sum/shufb/dma/brsl) et les compiler dans le runtime.
  (c) Cabler DMA (LS<->memoire principale) + channels (deja: spu_dma.h,
      spu_channels.c, spu_context.h) + la synchro de completion qui debloque le
      scheduler PPU (func_00038FBC) -> premier flip.
  (d) Backend RSX pour presenter.
NOTE: c'est un sous-systeme majeur (SPU+SPURS ~= ce que fait RPCS3). Decision de
scope a prendre avec l'utilisateur avant de s'y engager iteration par iteration.

## FRONTIERE PRECISE 2026-06-29b : le thread principal idle dans le scheduler de jobs

Sondes (iteration ~90-120s) :
- [38fbc-state] : func_00038FBC tourne avec obj=0x011E1998, *(obj+0x8C)=0xFFFFFFFF
  (-1) CONSTANT et flag @0x011DFDA8 = 0x00 CONSTANT. Donc la boucle prend toujours
  la branche "idle / pas de tache".
- [watch 0x011E1A24] (le champ obj+0x8C, le signal de tache) : seulement 2
  ecritures sur 120s, TOUTES = -1, par func_007ACD70 (<-func_00044AA0) et
  func_00058B24 (resets "pas de tache"). RIEN n'y ecrit jamais un id de tache
  valide (>=0).

CONCLUSION : le thread principal est coince dans une boucle de scheduler de jobs
(code overlay @0x012C4D10 appelant func_00038FBC ~22.5M fois) qui attend une
TACHE (obj+0x8C != -1) ou un EVENEMENT (flag 0x011DFDA8 != 0). Aucun producteur
ne les positionne -> idle infini -> jamais de 1er flip. Le producteur de
taches/evenements (probablement la boucle de frame pilotee par vblank, ou la
completion d'un worker/SPU) ne tourne pas. C'est le systeme de JOBS/EVENEMENTS,
pas le DC/SID (qui marche) ni le FIFO (draine).

RESULTAT watch flag 0x011DFDA8 : **0 ecriture en 130s, sur aucun thread**. Donc
le producteur de cet evenement NE TOURNE JAMAIS. Combine au signal de tache
(seulement reset a -1, jamais assigne), la conclusion est nette :

DIAGNOSTIC FINAL : le thread principal idle dans le scheduler de jobs en attente
d'un evenement/tache qu'AUCUN code ne produit. Le systeme EVENEMENTS/JOBS/VBLANK
ne tourne pas. Candidats du producteur manquant :
  - Livraison d'evenements VBLANK (lv2 : sys_event_queue + interruption vblank
    ~60Hz). Le jeu attend probablement un evenement vblank pour avancer sa frame.
    A implementer : une source vblank qui poste l'evenement / met le flag, et/ou
    appelle le handler guest via le dispatcher PPU.
  - Completion d'un worker/SPU (DC-script/SPU task) qui devrait poster le flag.
Aucun handler vblank/flip n'a ete enregistre (pas de cellGcmSetVBlankHandler dans
le log) -> le jeu utilise vraisemblablement sys_event_queue_receive pour le
vblank, ou poll un compteur. Verifier quelles files d'evenements / quels
sys_event_queue le jeu cree et attend, et y poster un evenement vblank periodique.

C'est la frontiere : le systeme d'evenements (vblank) + le backend RSX reel. Les
deux sont des sous-systemes substantiels. Le DC/SID, le FIFO (draine), le chargement
d'assets fonctionnent deja.

CONNEXION DECISIVE 2026-06-29c : `[cellGcmSys] SetFlipMode` est appele 341 fois en
130s (~2.6/s). Donc le jeu EXECUTE bien sa boucle de frame (341 frames) mais ne
FLIPPE jamais (SetFlipCommand=0) : chaque frame, le scheduler idle (~66000
iterations func_00038FBC) car AUCUN JOB n'est produit. Et aucun job n'est produit
parce qu'AUCUN NIVEAU/CONTENU n'est charge : 'world-test' a echoue -> pas de
contenu de frame -> boucle vide -> rien a flipper.

=> Le vrai verrou est le FLOW DE CHARGEMENT DE NIVEAU/MENU, pas le systeme
d'evenements (la boucle de frame TOURNE deja). Le jeu demande le niveau
'world-test' (defaut dev, via global *(r30-0x7F24)) au lieu du menu reel / d'un
vrai niveau. Le registre DC/SID a les definitions (misc-fx, decals... trouves),
mais 'world-test' n'y est pas (niveau dev non-shippe).

PROCHAIN POINT (revise, le plus prometteur) : trouver qui ECRIT le global de nom
de niveau *(r30-0x7F24)='world-test' (le flow boot/menu) et pourquoi il met le
defaut au lieu du vrai niveau de demarrage / menu. Sur PS3 retail, apres l'intro,
le jeu charge le MENU (un world avec l'UI). Soit le menu doit etre charge
automatiquement (nom de niveau a corriger), soit le flow attend une selection.
Trouver le vrai nom de niveau de demarrage (dans les paks/DC) et le faire charger
ferait tourner la boucle de frame avec du contenu -> 1er flip -> (avec backend RSX)
image visible. C'est plus accessible que reecrire le systeme d'evenements.

TRACE 2026-06-29d : 'world-test' est l'argument r4 (PAS r3) dans la chaine.
func_008BC86C et func_000428A0(@ligne lifted 57789) recoivent r3=REGISTRE
(0x01352E20 ; lu en string ca donne '1G5 ' = garbage). La chaine de l'erreur
world-test passe par func_000428A0+0xEE8D (site d'appel specifique dans cette
enorme fonction) -> func_008BC86C -> func_008BBF04 -> func_008BBDB8(r3=registre,
r4='world-test'). 'world-test' est construit au runtime (absent de l'EBOOT). Les
lookups d'effets (misc-fx/decals/particle/hud) REUSSISSENT ; seul 'world-test'
(niveau dev non-shippe) echoue. NEXT : sonde [levellookup] sur func_008BBDB8 avec
limite plus haute + match 'world' pour capturer le nameptr de 'world-test', puis
WATCH ce nameptr -> qui CONSTRUIT 'world-test' (flow boot/menu) -> y substituer le
vrai niveau de menu. Iteration ~120s/run -> grouper les sondes.

## CLARIFICATION 2026-06-29 : le registre DC/SID FONCTIONNE ; le verrou est l'AFFICHAGE

Sonde [levellookup] sur func_008BBDB8 (boot ~90s, iteration RAPIDE) : le registre
N'EST PAS vide. registry=0x01352E20, *(reg)=0x31473520 (pointeur heap valide), et
les lookups REUSSISSENT pour 'misc-fx', 'decals', 'particle', 'particle-shrub',
'hud'. Donc le systeme DC-script/SID (hash + registre) MARCHE. Seul 'world-test'
echoue : c'est un niveau DEV non-shippe (absent de paks.txt qui liste les ~700
vrais paks/niveaux). => 'world-test' n'est PAS le blocage profond que je
craignais.

De plus : APRES l'erreur world-test, le jeu CONTINUE — il fait
`[cellGcmSys] SetFlipMode(mode=2)`, genere des commandes GCM (4x gcm-overflow),
cree spu-fence + threads. Donc le jeu est dans une BOUCLE DE RENDU et world-test
est vraisemblablement NON-FATAL. Le vrai verrou pour voir quelque chose est le
PIPELINE D'AFFICHAGE : aucun backend RSX ne consomme le FIFO -> GET n'avance pas
-> la callback de flip/overflow attend -> le thread principal spin (func_00038FBC
+ formatter func_00D79450 ~573M = sprintf garbage, goulot de lenteur separe).

NOUVELLE PRIORITE (revise) : brancher la consommation du FIFO GCM + le flip
(avancer GET dans g_gcm.control, signaler la completion de flip) — backend null
d'abord pour debloquer la boucle de rendu, puis D3D12 pour rendre visible. C'est
le chemin le plus court vers une sortie a l'ecran maintenant que le DC/SID marche.
Iteration ~90s (le boot atteint SetFlipMode/world-test/gcm-overflow en ~90s).

## JALON LE PLUS AVANCE (2026-06-28f, boot 15 min)

Le jeu charge ses VRAIES donnees disque avec succes (fonts *.fnt, paks.txt,
banks.txt, pak23.txt, ss-audio-precache.txt, **movie1/** = cinematique d'intro,
boot1/NEWSAVE.PNG) — seuls echecs : DLC et http-cache (non critiques). Il cree
les threads NpTrophy, Save/Load, DCLoader. Puis il atteint le **CHARGEMENT DE
NIVEAU** et echoue avec :

    Level 'world-test' has no level definition!
    !!! Error loading level 'world-test' !!!

`world-test` est un niveau FALLBACK (defaut dev de Naughty Dog) ; il n'est ni
dans l'EBOOT ni dans les .txt disque. L'erreur "has no level definition" indique
que le **registre des definitions de niveaux est vide/incomplet** -> le jeu ne
trouve pas le vrai niveau de demarrage (menu/intro) et retombe sur world-test.

Cause probable : le traitement DC/banques (la phase func_00038FBC + le worker
DCLoader) ne peuple pas correctement le registre des niveaux. A noter : le
garde-fou pose dans func_00767A18 (borne >= 0x100000 = fin de boucle) a peut-etre
saute des donnees legitimes ; a re-verifier (les comptes 14.5M etaient des
pointeurs rodata = garbage, donc le garde-fou semblait correct, mais l'impact sur
les definitions de niveaux est a confirmer).

PROCHAIN POINT PRECIS : comprendre comment le jeu determine/charge les
definitions de niveaux (probablement un DC-script dans un .psarc des banques) et
pourquoi le registre est vide -> soit le DC processing est incomplet, soit une
donnee est sautee. C'est le dernier verrou connu avant un niveau jouable (puis
restera le pipeline d'affichage pour le rendre visible).

TRACE (sonde [level-fail], boot ~12 min) : la chaine d'appel est
func_00010230 -> func_00010354 -> func_00058B24 -> func_000428A0 ->
func_008BC86C (LoadLevel) -> func_008BBF04 -> func_008BBDB8 (lookup du registre,
imprime l'erreur). Dans func_000428A0 (b0000:57786-57793) le nom de niveau est
lu d'un GLOBAL : `r29 = *(r30 - 0x7F24)` puis `func_008BC86C(r29)`. Ce global
pointe sur la chaine 'world-test' (construite au runtime ; absente de l'EBOOT).
=> 'world-test' est le DEFAUT ; le vrai niveau de demarrage devrait ecrire ce
global avant func_000428A0, et/ou le registre interroge par func_008BBDB8 doit
contenir les definitions. Les deux pointent vers le pipeline DC-script (les
definitions de niveaux viennent de DC-scripts dans les .psarc des banques, non
peuples car le DC/SPU processing est incomplet - limite connue).

Pistes : (a) examiner func_008BBDB8 pour voir si le registre est totalement vide
ou si seul 'world-test' manque ; (b) trouver qui ecrit le global (r30-0x7F24)
pour voir si le vrai niveau devrait y etre ; (c) avancer le DC-script processing
pour peupler les definitions. Le formatter spin (func_00D79450/func_00D70B7C =
memcpy-append, ~573M appels = sprintf sur arg garbage, OOB 0xFFFFFFFF) est un
goulot de lenteur separe a borner aussi.

## Correctifs valides

- L'ELF charge et le code genere utilisent tous deux la mise a jour 1.19.
- La VM invitee reserve les 4 Go et les threads PPU/lwmutex/lwcond fonctionnent.
- Le VFS reconnait aussi les racines exactes comme `/dev_bdvd`, ce qui a debloque
  le chargement apres `boot1/coin-3d.dds`.
- `CellFsStat` utilise le layout PS3 compact de 52 octets et expose la vraie
  taille des fichiers a FIOS.
- Le bridge Edge execute le DEFLATE brut sur l'hote et reproduit le compteur et
  l'event flag de completion de la tache SPU.
- SPURS2 initialise un contexte de 8 Ko, cree 24 workloads avec des IDs valides,
  gere les attributs, `GetInfo`, Taskset2, LFQueue et les event flags.
- GCM initialise le contexte invite, les tables EA/IO, les labels et le controle.
  Le jeu mappe 98 Mo puis 26 Mo, configure les tiles/zcull et deux buffers
  `1280x720`, pitch 5120.
- Les ABI `CellVideoOutState` et `cellGameGetParam*` sont corrigees. Le log voit
  `BCES01175`, le niveau parental 7 et la resolution 720p.
- Les huit ports audio, XInput, clavier et souris sont initialises.
- `sys_prx_get_module_list` renvoie maintenant une vraie liste vide (`count=0`)
  au lieu de laisser le compteur et les IDs invites non initialises.
- Le tail-chain de `func_008AC438` pouvait revenir avec sa frame de 0x90 octets
  encore active. Le callsite restaure desormais `r1` et `r29-r31`; `main`
  conserve son `r30=0x01136880` et les ecarts de budget de 1 Mo ont disparu.
- `cellGameDataCheck` renvoie `CELL_GAME_RET_NONE` pour des game data absentes,
  puis `CreateGameData`, `SetParamString` et `ContentPermit` creent correctement
  `hdd0/game/BCES01175DATA2` et ses six icones.
- `/dev_hdd0` est monte a la racine `hdd0` au lieu d'etre ajoute sous le dossier
  du titre; les chemins `game/<title>` ne sont plus dupliques.

## Validation de reference

Build depuis une invite Visual Studio x64 :

```powershell
cmake --build build --config Release --target Uncharted3Recomp --parallel 4
```

Test depuis `projects/uncharted3` :

```powershell
.\build\Uncharted3Recomp.exe game\EBOOT_1.19.ELF
```

Le processus ne se termine pas seul pour le moment. Un test automatise doit le
couper apres la fenetre d'observation. Aucun processus de test ne doit rester
actif ensuite.

## Prochain obstacle

MISE A JOUR 2026-06-28b : la phase banks.txt est franchie. La sonde montre que
les banques AVANCENT (entry 0->6, pas de reprocess). L'OOM venait de certaines
entrees dont la base est corrompue (une table de pointeurs statique p.ex.
0x011D9038), si bien que la boucle de `func_00767A18` lisait sa borne
`*(base+idx*4+0x50)` comme un pointeur rodata (~0x00DDxxxx = millions) -> ~2 Mo
alloues du pool scratch -> "Out of memory in allocator" + spin. Garde-fou pose
dans `func_00767A18` (recompiled/ppu_recomp_b0011.cpp, juste avant
`goto loc_00767CF0`) : une borne >= 0x100000 est traitee comme fin de boucle.

Resultat : plus d'OOM, le thread principal devient ACTIF (~12M tail-calls /
heartbeat) et le boot atteint de NOUVEAUX jalons : creation des workers
`DCLoader` et `Save/Load Game Thread`, `[spu-fence]`, et surtout la **soumission
de commandes GCM** avec gestion du debordement du command buffer
(`[gcm-overflow]`, callback 0x011A49E8). Le jeu genere donc des commandes RSX.

Obstacle courant : (1) `func_00038FBC` est appele en boucle (~22.5M) avec des
args constants (r3=r4=r5=0) depuis du code overlay a 0x012C4D10 (BSS de seg2,
charge au runtime) — probablement un poll attendant le RSX/worker ; (2) le
command buffer GCM deborde sans backend RSX pour le consommer (la callback
reinitialise le pointeur mais rien n'execute le FIFO).

Prochaine etape (doc etape 3) : initialiser le backend RSX null + pompe de
messages pour consumer le FIFO ; verifier si cela debloque le poll func_00038FBC.

MISE A JOUR 2026-06-28c (affinage) : `cellGcmSetFlipCommand` n'est JAMAIS appele
(0x) -> le jeu n'a pas encore atteint le PREMIER FLIP ; il est bloque dans la
phase de CHARGEMENT, pas dans la boucle de rendu en regime. Le thread principal
boucle dans `func_00038FBC` (poll, code overlay a 0x012C4D10) en attendant la fin
du chargement. Le worker `DCLoader` (OPD 0x011B4EC0 -> code func_009D4FDC ->
func_009D4B94, arg/queue 0x13A4810) est cree ~100x (slot THREAD 14 reutilise) :
chaque instance traite UN item puis fait thread_exit, et le jeu en respawn un
autre. A determiner (test boot long en cours) : est-ce 100 items DIFFERENTS
(progression lente du chargement) ou le MEME item retente (stuck) ? Le backend
RSX null (GET=PUT) ne debloque pas ce poll (il n'attend pas GET mais la fin du
chargement). Prochain point precis : `func_009D4B94` (corps du worker DCLoader) —
pourquoi il sort sans faire progresser la queue, ou s'il depend d'une tache SPU
non executee (limite connue).

MISE A JOUR 2026-06-28d (boot 5 min) : le DCLoader plafonne a 103 (pool unique,
pas un respawn), et `func_00038FBC` est une boucle FINIE de 22.5M iterations qui
se TERMINE (~60 s) — le jeu PROGRESSE ensuite. Nouveau mur a la phase suivante :
le formatter spinne — `func_00D79450` (petit helper : appelle func_00D70B7C,
renvoie une longueur) est appele **573 MILLIONS de fois** par son appelant (la
boucle de formatage), avec des acces OOB a **0xFFFFFFFF** (pointeur -1 passe a un
printf). Meme classe que l'ancien func_00D865DC (formatter sur longueur garbage).
Le 1er flip n'est toujours pas atteint. Le boot avance de mur en mur dans l'init
(banks -> DCLoader -> func_00038FBC -> formatter), chacun tres lent. Prochain
point : trouver l'appelant de func_00D79450 (la boucle du formatter) et la source
du pointeur -1, ou poser un garde-fou de longueur comme pour les banques.

## Ordre de travail

1. Tracer la progression dans `banks.txt` et corriger la boucle/ABI responsable
   si le meme element est retraite.
2. Obtenir une boucle principale stable et la premiere commande de flip.
3. Initialiser le backend RSX null et sa pompe de messages pour prouver le FIFO.
4. Completer les appels input `GetInfo/GetData` et la cadence audio.
5. Ajouter les fallbacks SPU cibles pour audio, animation et physique.
6. Passer ensuite au backend D3D12 et a la traduction des shaders RSX.

## Limites connues

- 9 NID distincts restent non resolus sur le boot de reference. Plusieurs sont
  des chemins hors-ligne ou de destruction NP/trophees, mais les sorties de
  structures doivent etre traitees avant tout faux succes.
- `config.toml` annonce `backend = "null"`, mais cette option n'est pas encore
  consommee par le point d'entree UC3.
- Le bridge audio fournit des buffers invites valides sans mixage cadence complet.
- Les taches SPU generiques et le FIFO RSX ne sont pas encore executes.
- Le lien emet des avertissements de symboles `sceNpCommerce2*` dupliques ; ils
  ne bloquent pas le build mais devront etre nettoyes.

## Legal

Utiliser uniquement une copie legalement acquise du jeu et ses propres cles.
Ne pas redistribuer l'ELF, les assets ou le binaire recompile.
