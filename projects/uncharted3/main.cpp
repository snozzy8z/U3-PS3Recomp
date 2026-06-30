/*
 * Uncharted 3 (BCES01175) - point d'entree du portage ps3recomp.
 *
 * Adapte de runtime/ppu/tests/boot_main.cpp (reference de cablage du SDK).
 * Relie : code recompile (recompiled/ppu_recomp_*.cpp) + table
 * (ppu_recomp_table.cpp) + loader (ppu_loader.cpp) + HLE + syscalls lv2, puis
 * dispatche le point d'entree reel du jeu (OPD e_entry de l'EBOOT).
 *
 * Boot :
 *   1. allouer la memoire invitee (vm_base)
 *   2. ppu_load_elf  -> charge les PT_LOAD, renvoie l'OPD d'entree
 *   3. ppu_recomp_register -> branche function_table[] (83031 fonctions)
 *   4. ppu_hle_init / sysprx / fs / lv2  -> firmware HLE + syscalls
 *   5. ppu_run -> resout l'OPD (code+TOC), pose r1/r2, execute l'entree
 *
 * Avec break_on_unimplemented, l'execution avance dans le vrai code de boot
 * d'Uncharted 3 jusqu'au premier NID/syscall non implemente (loggue) : c'est
 * la liste de travail pour stubs.cpp / les bridges HLE.
 */
#include "ppu_recomp.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* API fournie par la bibliotheque runtime ps3recomp. */
extern "C" {
uint32_t ppu_load_elf(const char* path);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
void     lv2_init_syscalls(void);
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
extern const char* ppu_vfs_root;
extern uint64_t    ppu_vm_size;
}

/* Symbole attendu par le runtime + les libs HLE. */
extern "C" uint8_t* vm_base = nullptr;

/* Espace invite : on mappe TOUT l'espace d'adressage 32 bits du PS3 (4 Go) a
 * plat, comme decrit dans docs/ARCHITECTURE.md (host_ptr = vm_base + addr). Le
 * code lifte accede a la memoire de deux facons : via vm_read/write (bornes
 * verifiees) ET, pour les vecteurs VMX/AltiVec, par un memcpy direct sur
 * vm_base+ea SANS verification. Une VM trop petite faisait donc segfault l'hote
 * des qu'un acces VMX visait une adresse guest haute (RSX 0xC0000000, etc.).
 * On reserve les 4 Go et on engage les pages a la demande (VEH), ce qui couvre
 * tous les acces sans consommer 4 Go de RAM d'emblee. */
#define VM_SIZE    0x100000000ull   /* 4 GB flat guest address space */
#define STACK_TOP  0x0FF00000u

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static LONG CALLBACK vm_commit_handler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uint8_t* fault = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
        if (vm_base && fault >= vm_base && fault < vm_base + VM_SIZE) {
            /* Commit the 64 KB region around the faulting guest address. */
            uintptr_t base = (uintptr_t)fault & ~(uintptr_t)0xFFFF;
            if (VirtualAlloc((void*)base, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
            MEMORY_BASIC_INFORMATION mbi{};
            VirtualQuery(fault, &mbi, sizeof(mbi));
            fprintf(stderr,
                    "[vm] commit failed: guest=0x%08llX host=%p error=%lu state=0x%lX protect=0x%lX\n",
                    (unsigned long long)(fault - vm_base), fault, GetLastError(),
                    (unsigned long)mbi.State, (unsigned long)mbi.Protect);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* Racine VFS depuis un EBOOT place sous PS3_GAME/USRDIR ou directement sous
 * le dossier titre. $PS3_VFS_ROOT reste prioritaire. */
static char s_vfs_root[1024];
static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }
    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    s_vfs_root[sizeof s_vfs_root - 1] = 0;
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';

    char* marker = strstr(s_vfs_root, "/PS3_GAME/USRDIR/");
    if (marker) {
        *marker = 0; /* Standard disc layout: root contains PS3_GAME. */
    } else if ((marker = strstr(s_vfs_root, "/USRDIR/")) != nullptr) {
        *marker = 0; /* Extracted title layout: root contains USRDIR. */
    } else {
        strcpy(s_vfs_root, "hdd0/game/BCES01175");
    }
    ppu_vfs_root = s_vfs_root;
}

int main(int argc, char** argv)
{
    /* Bring-up : stdout non-bufferise pour voir les logs HLE en direct
     * (sinon perdus si le process est tue / boucle sans flush). */
    setvbuf(stdout, nullptr, _IONBF, 0);

    /* The lifted code was generated from the 1.19 update executable. Loading
     * the retail 1.00 ELF uses different OPD/TOC/data addresses and cannot be
     * mixed with this recompiled image. */
    const char* eboot = (argc >= 2) ? argv[1] : "game/EBOOT_1.19.ELF";
    printf("=== ps3recomp : Uncharted 3 (BCES01175) ===\n");
    printf("[boot] EBOOT: %s\n", eboot);

#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_handler);
    vm_base = (uint8_t*)VirtualAlloc(nullptr, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    if (!vm_base) { fprintf(stderr, "[boot] echec reserve VM 4GB\n"); return 1; }
    fprintf(stderr, "[boot] vm_base=%p, guest span=4GB\n", (void*)vm_base);
    /* Commit the low region eagerly (image + stack + heap live here); the rest
     * commits lazily via the VEH on first touch. */
    if (!VirtualAlloc(vm_base, 0x51000000u, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "[boot] echec commit VM bas\n"); return 1;
    }
#else
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { fprintf(stderr, "[boot] echec alloc VM\n"); return 1; }
#endif
    ppu_vm_size = VM_SIZE;

    uint32_t entry = ppu_load_elf(eboot);
    if (!entry) { fprintf(stderr, "[boot] echec chargement ELF\n"); free(vm_base); return 1; }

    derive_vfs_root(eboot);
    printf("[boot] racine VFS: %s\n", ppu_vfs_root);

    printf("[boot] ppu_recomp_register...\n");  ppu_recomp_register();
    printf("[boot] ppu_hle_init...\n");          ppu_hle_init();
    printf("[boot] ppu_sysprx_register...\n");   ppu_sysprx_register();
    printf("[boot] ppu_fs_register...\n");       ppu_fs_register();
    printf("[boot] lv2_init_syscalls...\n");     lv2_init_syscalls();
    printf("[boot] init done.\n");

    printf("\n[boot] dispatch entree OPD 0x%08X (stack top 0x%08X)\n\n", entry, STACK_TOP);
    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run a retourne %d\n", rc);

    free(vm_base);
    return rc;
}
