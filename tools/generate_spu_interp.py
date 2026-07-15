#!/usr/bin/env python3
"""Génère un interpréteur SPU en C (fetch/decode/execute) à partir de spu_decode
(décodage) + Lifter._translate (sémantique). Approche RPCS3: interpréter le LS au
runtime au lieu de lifter statiquement — résout jump-tables/overlays/code DMA sans
lifting. Réutilise runtime/spu/spu_helpers.h (déjà toute la sémantique).

Stratégie: pour chaque mnémonique connu, décoder une instruction CANONIQUE
(rt=0, ra=1, rb=2, rc=3, immédiat placeholder), appeler _translate, puis remplacer
les registres/immédiats fixes par les champs décodés au runtime, et les
branchements-vers-fonction-liftée par une mise à jour de ctx->pc.
"""
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spu_disasm as D
from spu_disasm import spu_decode, SPUInstruction

# Importer le Lifter (pour _translate) — on lui donne un contexte minimal.
import spu_lifter as L

# Registres canoniques distincts (choisis pour ne pas collisionner avec des
# constantes fréquentes): rt=3, ra=5, rb=7, rc/rt4=9.
CT, CA, CB, CC = 3, 5, 7, 9

def encode(fmt, op, rt=CT, ra=CA, rb=CB, rc=CC, imm=0):
    """Réencode une instruction pour un format/opcode donné (inverse de spu_decode)."""
    if fmt == "RR":      # op11 << 21 | rb<<14 | ra<<7 | rt
        return (op << 21) | (rb << 14) | (ra << 7) | rt
    if fmt == "RRR":     # op4<<28 | rt4(rc)<<21 | rb<<14 | ra<<7 | rt
        return (op << 28) | (rc << 21) | (rb << 14) | (ra << 7) | rt
    if fmt == "RI10":    # op8<<24 | i10<<14 | ra<<7 | rt
        return (op << 24) | ((imm & 0x3FF) << 14) | (ra << 7) | rt
    if fmt == "RI16":    # op9<<23 | i16<<7 | rt
        return (op << 23) | ((imm & 0xFFFF) << 7) | rt
    if fmt == "RI18":    # op7<<25 | i18<<7 | rt
        return (op << 25) | ((imm & 0x3FFFF) << 7) | rt
    if fmt == "RI7":     # op11<<21 | i7<<14 | ra<<7 | rt
        return (op << 21) | ((imm & 0x7F) << 14) | (ra << 7) | rt
    if fmt == "RI8":     # op10<<22 | i8<<14 | ra<<7 | rt
        return (op << 22) | ((imm & 0xFF) << 14) | (ra << 7) | rt
    raise ValueError(fmt)

# Placeholder immédiat unique par largeur, pour substitution runtime.
IMM10 = 0x155      # motif reconnaissable dans i10
IMM16 = 0x5A5A
IMM18 = 0x15555
IMM7  = 0x2B
IMM8  = 0x5B

def subst_regs_imm(c, imm_expr=None, imm_base=None):
    """Remplace gpr[canonique]->gpr[champ] et l'immédiat placeholder->imm_expr.
    Le désassembleur pré-échelonne certains immédiats (lqd/stqd ×16, lqa ×4);
    on remplace donc les formes ÉCHELONNÉES d'abord (sous-chaînes plus longues)
    par (imm_expr*scale), puis la forme brute."""
    c = c.replace(f"ctx->gpr[{CT}]", "ctx->gpr[rt]")
    c = c.replace(f"ctx->gpr[{CA}]", "ctx->gpr[ra]")
    c = c.replace(f"ctx->gpr[{CB}]", "ctx->gpr[rb]")
    c = c.replace(f"ctx->gpr[{CC}]", "ctx->gpr[rc]")
    if imm_expr and imm_base is not None:
        for scale in (16, 8, 4, 2):           # formes échelonnées en premier
            c = c.replace(f"0x{imm_base*scale:X}", f"({imm_expr}*{scale})")
        c = c.replace(f"0x{imm_base:X}", imm_expr)   # forme brute ensuite
        c = c.replace(str(imm_base), imm_expr)
    return c

def main():
    # Un Lifter minimal juste pour _translate (pas de flux de contrôle réel).
    lifter = L.SPULifter.__new__(L.SPULifter)
    for attr, val in (("func_return_regs", {}), ("func_return_sites", {}),
                      ("unsupported", {}), ("computed_branches", False),
                      ("symbol_prefix", ""), ("manual_link_branches", set()),
                      ("branch_targets", set()), ("call_targets", set()),
                      ("func_starts", set()), ("trace", False)):
        setattr(lifter, attr, val)
    class _F:
        start_addr = 0
    func = _F()

    cases_rr, cases_rrr, cases_ri10, cases_ri16, cases_ri18, cases_ri7 = ([] for _ in range(6))
    covered, skipped = 0, []

    # Manuel = seulement ce que _translate ne peut PAS rendre pour l'interpréteur:
    #  - branchements réels (pc change; _translate émet fn(ctx)/goto loc_ liftés),
    #  - channels (doivent être décodés AVANT RI10 → switch séparé),
    #  - iret (rare; halt sûr). Les hints/nops/stop/hbra/hbrr sont laissés à emit()
    #    qui les place dans le BON switch de format (hbra/hbrr=RI18, stop/nop/hbr=RR).
    MANUAL = {"bi","bisl","biz","binz","bihz","bihnz","bisled","iret",
              "br","bra","brsl","brasl","brz","brnz","brhz","brhnz",
              "rdch","wrch","rchcnt",
              # load/store absolus/relatifs: le désassembleur pré-échelonne
              # l'immédiat ((i16<<2)&0x3FFF0 pour a-forms, i16*4+addr pour r-forms),
              # ce que subst_regs_imm ne peut pas retrouver → adresse figée. Manuel.
              "stqa","lqa","stqr","lqr"}

    def emit(table, fmt, imm_ph, imm_expr):
        nonlocal covered
        out = []
        for op, mn in sorted(table.items()):
            if mn in MANUAL:
                continue
            raw = encode(fmt, op, imm=imm_ph)
            insn = spu_decode(raw, addr=0x1000)
            if insn.mnemonic != mn:
                skipped.append((mn, "decode-mismatch")); continue
            try:
                c = lifter._translate(insn, func)
            except Exception as e:
                skipped.append((mn, f"translate:{e}")); continue
            # Branchements: _translate émet "ctx->pc = ...; func(ctx); return;" ou
            # "goto ...". Pour l'interpréteur on garde seulement l'update de pc.
            c = re.sub(r";\s*[A-Za-z_]\w*\(ctx\);\s*return;", "; nextpc = ctx->pc; goto advance;", c)
            c = re.sub(r"spu_indirect_branch\(ctx\);\s*return;", "goto advance;", c)
            c = c.replace("return;", "goto advance;")
            c = subst_regs_imm(c, imm_expr, imm_ph)
            # CRUCIAL: chaque case exécuté doit `goto advance` (les switches sont
            # chaînés; un simple `break` retombe sur `return -1` = faux "non géré").
            cs = c.rstrip()
            if not (cs.endswith("goto advance;") or cs.endswith("return;")
                    or cs.endswith("return 0;") or cs.endswith("return -1;")):
                c = cs + " goto advance;"
            out.append(f"        case 0x{op:X}: {c} /* {mn} */")
            covered += 1
        return out

    rr_table = getattr(D, "SPU_RR", D.RR_TABLE)  # SPU_RR = 122 opcodes (le vrai table)
    cases_rr   = emit(rr_table,     "RR",   0,      None)
    cases_rrr  = emit(D.RRR_TABLE,  "RRR",  0,      None)
    cases_ri10 = emit(D.RI10_TABLE, "RI10", IMM10,  "i10")
    cases_ri16 = emit(D.RI16_TABLE, "RI16", IMM16,  "i16")
    cases_ri18 = emit(D.RI18_TABLE, "RI18", IMM18,  "i18")

    # ---- Contrôle de flux (branchements) + channels : émis à la main, car ils
    # modifient ctx->pc / status (le _translate liftée émet des goto/return vers
    # des fonctions liftées qui n'existent pas dans l'interpréteur). Sémantique
    # et convention de slot (_u32[0] = slot préféré) recopiées de spu_lifter.py. ----
    nonlocal_covered = [covered]
    def manual_ctrl():
        ri16_ops = {v: k for k, v in D.RI16_TABLE.items()}
        rr_ops   = {v: k for k, v in rr_table.items()}
        add16, addrr = [], []
        REL, ABS = "pc + (uint32_t)(i16*4)", "(uint32_t)(i16*4)"
        ri16_c = {
            "br":    f"nextpc = {REL}; goto advance;",
            "bra":   f"nextpc = {ABS}; goto advance;",
            "brsl":  f"ctx->gpr[rt] = spu_splat_u32(pc+4); nextpc = {REL}; goto advance;",
            "brasl": f"ctx->gpr[rt] = spu_splat_u32(pc+4); nextpc = {ABS}; goto advance;",
            "brz":   f"if (ctx->gpr[rt]._u32[0]==0) nextpc = {REL}; goto advance;",
            "brnz":  f"if (ctx->gpr[rt]._u32[0]!=0) nextpc = {REL}; goto advance;",
            "brhz":  f"if (ctx->gpr[rt]._u16[0]==0) nextpc = {REL}; goto advance;",
            "brhnz": f"if (ctx->gpr[rt]._u16[0]!=0) nextpc = {REL}; goto advance;",
            # load/store quadword absolus (a-form: adresse = (i16<<2)&0x3FFF0)
            # et relatifs au pc (r-form: adresse = (pc+i16*4)&0x3FFF0). Adresse
            # calculée au RUNTIME (cf. spu_disasm.spu_decode lignes 363-383).
            "stqa":  "spu_ls_write128(ctx, (uint32_t)((((insn>>7)&0xFFFF)<<2)&0x3FFF0), ctx->gpr[rt]); goto advance;",
            "lqa":   "ctx->gpr[rt] = spu_ls_read128(ctx, (uint32_t)((((insn>>7)&0xFFFF)<<2)&0x3FFF0)); goto advance;",
            "stqr":  "spu_ls_write128(ctx, (pc + (uint32_t)(i16*4)) & 0x3FFF0u, ctx->gpr[rt]); goto advance;",
            "lqr":   "ctx->gpr[rt] = spu_ls_read128(ctx, (pc + (uint32_t)(i16*4)) & 0x3FFF0u); goto advance;",
        }
        rr_c = {
            "bi":    "nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance;",
            "bisl":  "ctx->gpr[rt] = spu_splat_u32(pc+4); nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance;",
            "biz":   "if (ctx->gpr[rt]._u32[0]==0) nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance;",
            "binz":  "if (ctx->gpr[rt]._u32[0]!=0) nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance;",
            "bihz":  "if (ctx->gpr[rt]._u16[0]==0) nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance;",
            "bihnz": "if (ctx->gpr[rt]._u16[0]!=0) nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance;",
            "iret":  "return 0; /* iret non supporté: halt */",
        }
        for mn, c in ri16_c.items():
            if mn in ri16_ops:
                add16.append(f"        case 0x{ri16_ops[mn]:X}: {c} /* {mn} */")
                nonlocal_covered[0] += 1
        for mn, c in rr_c.items():
            if mn in rr_ops:
                addrr.append(f"        case 0x{rr_ops[mn]:X}: {c} /* {mn} */")
                nonlocal_covered[0] += 1
        # Channels (op11 fixes: rdch=0x00D, wrch=0x10D, rchcnt=0x00F). Doivent être
        # décodés AVANT RI10 (wrch op11=0x10D partage op8=0x21 avec shli) — switch séparé.
        addch = [
            "        case 0x00D: ctx->gpr[rt] = spu_rdch(ctx, ra); goto advance; /* rdch */",
            "        case 0x10D: spu_wrch(ctx, ra, ctx->gpr[rt]); goto advance; /* wrch */",
            "        case 0x00F: ctx->gpr[rt] = spu_splat_u32(spu_rchcnt(ctx, ra)); goto advance; /* rchcnt */",
        ]
        nonlocal_covered[0] += 3
        return add16, addrr, addch
    add16, addrr, cases_ch = manual_ctrl()
    cases_ri16 += add16
    cases_rr   += addrr

    # RI8 (op10, conversions float<->int) et RI7 (op11, décalages/rotations immédiats)
    # — tables locales à spu_decode, recopiées ici (voir spu_disasm.spu_decode).
    RI8_TABLE = {0b0111011000: "cflts", 0b0111011001: "cfltu",
                 0b0111011010: "csflt", 0b0111011011: "cuflt"}
    RI7_TABLE = {0x78: "roti", 0x79: "rotmi", 0x7a: "rotmai", 0x7b: "shli",
                 0x7c: "rothi", 0x7d: "rothmi", 0x7e: "rotmahi", 0x7f: "shlhi",
                 0x1F4: "cbd", 0x1F5: "chd", 0x1F6: "cwd", 0x1F7: "cdd",
                 0x1F8: "rotqbii", 0x1F9: "rotqmbii", 0x1FB: "shlqbii",
                 0x1FC: "rotqbyi", 0x1FD: "rotqmbyi", 0x1FF: "shlqbyi"}
    cases_ri8 = emit(RI8_TABLE, "RI8", IMM8, "i8")
    cases_ri7 = emit(RI7_TABLE, "RI7", IMM7, "i7u")
    covered = nonlocal_covered[0]

    hdr = f"""/* Auto-généré par tools/generate_spu_interp.py — NE PAS ÉDITER À LA MAIN.
 * Interpréteur SPU (fetch/decode/execute), approche RPCS3: exécute le LS au
 * runtime au lieu de lifter statiquement. Résout jump-tables/overlays/code DMA.
 * Couvre {covered} mnémoniques; les non couverts renvoient -1 (à compléter). */
#include "ps3emu/ps3types.h"
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdint.h>
#include <stdio.h>

void spu_indirect_branch(spu_context* ctx);
u128 spu_rdch(spu_context* ctx, uint32_t ch);
void spu_wrch(spu_context* ctx, uint32_t ch, u128 v);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch);

typedef void (*spu_interp_pc_hook)(spu_context* ctx, uint32_t pc);
static spu_context* g_spu_interp_hook_ctx;
static spu_interp_pc_hook g_spu_interp_hook;

void spu_interp_set_pc_hook(spu_context* ctx, spu_interp_pc_hook hook) {{
    g_spu_interp_hook_ctx = ctx;
    g_spu_interp_hook = hook;
}}

static inline uint32_t spu_ls_rd32(spu_context* ctx, uint32_t a) {{
    const uint8_t* p = ctx->ls + (a & (SPU_LS_SIZE - 1));
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}}

/* Retourne 0 si un STOP/halt propre a terminé, -1 sur opcode non géré (pc laissé
 * sur l'instruction fautive pour diagnostic/complétion). max_insns borne la boucle. */
int spu_interp_run(spu_context* ctx, long max_insns) {{
    for (long _n = 0; _n < max_insns; ++_n) {{
        if (ctx->status != SPU_STATUS_RUNNING) return 0;
        uint32_t pc   = ctx->pc & (SPU_LS_SIZE - 1);
        if (g_spu_interp_hook && ctx == g_spu_interp_hook_ctx)
            g_spu_interp_hook(ctx, pc);
        uint32_t insn = spu_ls_rd32(ctx, pc);
        uint32_t rt = insn & 0x7F, ra = (insn>>7)&0x7F, rb = (insn>>14)&0x7F, rc = (insn>>21)&0x7F;
        int32_t  i10 = (int32_t)(((insn>>14)&0x3FF) ^ 0x200) - 0x200;
        int32_t  i16 = (int32_t)(int16_t)((insn>>7)&0xFFFF);
        uint32_t i18 = (insn>>7)&0x3FFFF;
        int32_t  i7  = (int32_t)(((insn>>14)&0x7F) ^ 0x40) - 0x40;
        uint32_t i7u = (insn>>14)&0x7F;      /* RI7: décalage/rotation immédiat (champ rb) */
        uint32_t i8  = (insn>>14)&0xFF;       /* RI8: échelle de conversion float<->int */
        uint32_t op4=(insn>>28)&0xF, op7=(insn>>25)&0x7F, op8=(insn>>24)&0xFF, op9=(insn>>23)&0x1FF;
        uint32_t op10=(insn>>22)&0x3FF, op11=(insn>>21)&0x7FF;
        uint32_t nextpc = pc + 4;
        (void)rc;(void)i10;(void)i16;(void)i18;(void)i7;(void)i7u;(void)i8;(void)op7;(void)op8;(void)op9;(void)op10;
"""

    def sw(comment, var, cases):
        return (f"        /* {comment} */\n        switch ({var}) {{\n"
                + "\n".join(cases) + "\n        default: break; }\n")
    # Ordre IDENTIQUE à spu_decode: RRR, RI18, RI16, channels, RI10, RI8, RR, RI7.
    body  = sw("RRR (op4)",       "op4",  cases_rrr)
    body += sw("RI18 (op7)",      "op7",  cases_ri18)
    body += sw("RI16 (op9)",      "op9",  cases_ri16)
    body += sw("channels (op11, AVANT RI10)", "op11", cases_ch)
    body += sw("RI10 (op8)",      "op8",  cases_ri10)
    body += sw("RI8 (op10)",      "op10", cases_ri8)
    body += sw("RR (op11)",       "op11", cases_rr)
    body += sw("RI7 (op11)",      "op11", cases_ri7)

    ftr = """        {   /* aucun format n'a matché: opcode réellement non géré */
            static int _u = 0;
            if (_u < 48) { _u++;
                fprintf(stderr, "[spu-interp] NON GERE pc=0x%05X insn=0x%08X "
                    "op4=0x%X op7=0x%02X op8=0x%02X op9=0x%03X op10=0x%03X op11=0x%03X\\n",
                    pc, insn, op4, op7, op8, op9, op10, op11);
            }
        }
        return -1;
    advance:
        ctx->pc = nextpc;
    }
    return 0; /* budget d'instructions épuisé */
}
"""
    out = hdr + body + ftr
    outpath = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "runtime", "spu", "spu_interp.c")
    with open(outpath, "w", encoding="utf-8") as f:
        f.write(out)
    print(f"Écrit {outpath}")
    print(f"  {covered} mnémoniques couverts")
    if skipped:
        print(f"  {len(skipped)} skipped: {skipped[:10]}")

if __name__ == "__main__":
    main()
