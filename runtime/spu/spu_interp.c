/* Auto-généré par tools/generate_spu_interp.py — NE PAS ÉDITER À LA MAIN.
 * Interpréteur SPU (fetch/decode/execute), approche RPCS3: exécute le LS au
 * runtime au lieu de lifter statiquement. Résout jump-tables/overlays/code DMA.
 * Couvre 171 mnémoniques; les non couverts renvoient -1 (à compléter). */
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

void spu_interp_set_pc_hook(spu_context* ctx, spu_interp_pc_hook hook) {
    g_spu_interp_hook_ctx = ctx;
    g_spu_interp_hook = hook;
}

static inline uint32_t spu_ls_rd32(spu_context* ctx, uint32_t a) {
    const uint8_t* p = ctx->ls + (a & (SPU_LS_SIZE - 1));
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

/* Retourne 0 si un STOP/halt propre a terminé, -1 sur opcode non géré (pc laissé
 * sur l'instruction fautive pour diagnostic/complétion). max_insns borne la boucle. */
int spu_interp_run(spu_context* ctx, long max_insns) {
    for (long _n = 0; _n < max_insns; ++_n) {
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
        /* RRR (op4) */
        switch (op4) {
        case 0x8: ctx->gpr[rc] = spu_selb(ctx->gpr[ra], ctx->gpr[rb], ctx->gpr[rt]); goto advance; /* selb */
        case 0xB: ctx->gpr[rc] = spu_shufb(ctx->gpr[ra], ctx->gpr[rb], ctx->gpr[rt]); goto advance; /* shufb */
        case 0xC: ctx->gpr[rc] = spu_mpya(ctx->gpr[ra], ctx->gpr[rb], ctx->gpr[rt]); goto advance; /* mpya */
        case 0xD: ctx->gpr[rc] = spu_fnms(ctx->gpr[ra], ctx->gpr[rb], ctx->gpr[rt]); goto advance; /* fnms */
        case 0xE: ctx->gpr[rc] = spu_fma(ctx->gpr[ra], ctx->gpr[rb], ctx->gpr[rt]); goto advance; /* fma */
        case 0xF: ctx->gpr[rc] = spu_fms(ctx->gpr[ra], ctx->gpr[rb], ctx->gpr[rt]); goto advance; /* fms */
        default: break; }
        /* RI18 (op7) */
        switch (op7) {
        case 0x8: /* branch hint (ignored) */; goto advance; /* hbra */
        case 0x9: /* branch hint (ignored) */; goto advance; /* hbrr */
        case 0x21: ctx->gpr[rt] = spu_ila(i18); goto advance; /* ila */
        default: break; }
        /* RI16 (op9) */
        switch (op9) {
        case 0x65: ctx->gpr[rt] = spu_fsmbi(i16); goto advance; /* fsmbi */
        case 0x81: ctx->gpr[rt] = spu_il(i16); goto advance; /* il */
        case 0x82: ctx->gpr[rt] = spu_ilhu(i16); goto advance; /* ilhu */
        case 0x83: ctx->gpr[rt] = spu_ilh(i16); goto advance; /* ilh */
        case 0xC1: ctx->gpr[rt] = spu_iohl(ctx->gpr[rt], i16); goto advance; /* iohl */
        case 0x64: nextpc = pc + (uint32_t)(i16*4); goto advance; /* br */
        case 0x60: nextpc = (uint32_t)(i16*4); goto advance; /* bra */
        case 0x66: ctx->gpr[rt] = spu_splat_u32(pc+4); nextpc = pc + (uint32_t)(i16*4); goto advance; /* brsl */
        case 0x62: ctx->gpr[rt] = spu_splat_u32(pc+4); nextpc = (uint32_t)(i16*4); goto advance; /* brasl */
        case 0x40: if (ctx->gpr[rt]._u32[0]==0) nextpc = pc + (uint32_t)(i16*4); goto advance; /* brz */
        case 0x42: if (ctx->gpr[rt]._u32[0]!=0) nextpc = pc + (uint32_t)(i16*4); goto advance; /* brnz */
        case 0x44: if (ctx->gpr[rt]._u16[0]==0) nextpc = pc + (uint32_t)(i16*4); goto advance; /* brhz */
        case 0x46: if (ctx->gpr[rt]._u16[0]!=0) nextpc = pc + (uint32_t)(i16*4); goto advance; /* brhnz */
        case 0x41: spu_ls_write128(ctx, (uint32_t)((((insn>>7)&0xFFFF)<<2)&0x3FFF0), ctx->gpr[rt]); goto advance; /* stqa */
        case 0x61: ctx->gpr[rt] = spu_ls_read128(ctx, (uint32_t)((((insn>>7)&0xFFFF)<<2)&0x3FFF0)); goto advance; /* lqa */
        case 0x47: spu_ls_write128(ctx, (pc + (uint32_t)(i16*4)) & 0x3FFF0u, ctx->gpr[rt]); goto advance; /* stqr */
        case 0x67: ctx->gpr[rt] = spu_ls_read128(ctx, (pc + (uint32_t)(i16*4)) & 0x3FFF0u); goto advance; /* lqr */
        default: break; }
        /* channels (op11, AVANT RI10) */
        switch (op11) {
        case 0x00D: ctx->gpr[rt] = spu_rdch(ctx, ra); goto advance; /* rdch */
        case 0x10D: spu_wrch(ctx, ra, ctx->gpr[rt]); goto advance; /* wrch */
        case 0x00F: ctx->gpr[rt] = spu_splat_u32(spu_rchcnt(ctx, ra)); goto advance; /* rchcnt */
        default: break; }
        /* RI10 (op8) */
        switch (op8) {
        case 0x4: ctx->gpr[rt] = spu_ori(ctx->gpr[ra], i10); goto advance; /* ori */
        case 0x5: ctx->gpr[rt] = spu_orhi(ctx->gpr[ra], i10); goto advance; /* orhi */
        case 0x6: ctx->gpr[rt] = spu_orbi(ctx->gpr[ra], i10); goto advance; /* orbi */
        case 0xC: ctx->gpr[rt] = spu_sfi(ctx->gpr[ra], i10); goto advance; /* sfi */
        case 0xD: ctx->gpr[rt] = spu_sfhi(ctx->gpr[ra], i10); goto advance; /* sfhi */
        case 0x14: ctx->gpr[rt] = spu_andi(ctx->gpr[ra], i10); goto advance; /* andi */
        case 0x15: ctx->gpr[rt] = spu_andhi(ctx->gpr[ra], i10); goto advance; /* andhi */
        case 0x16: ctx->gpr[rt] = spu_andbi(ctx->gpr[ra], i10); goto advance; /* andbi */
        case 0x1C: ctx->gpr[rt] = spu_ai(ctx->gpr[ra], i10); goto advance; /* ai */
        case 0x1D: ctx->gpr[rt] = spu_ahi(ctx->gpr[ra], i10); goto advance; /* ahi */
        case 0x24: spu_ls_write128(ctx, ctx->gpr[ra]._u32[0] + (i10*16), ctx->gpr[rt]); goto advance; /* stqd */
        case 0x34: ctx->gpr[rt] = spu_ls_read128(ctx, ctx->gpr[ra]._u32[0] + (i10*16)); goto advance; /* lqd */
        case 0x44: ctx->gpr[rt] = spu_xori(ctx->gpr[ra], i10); goto advance; /* xori */
        case 0x4C: ctx->gpr[rt] = spu_cgti(ctx->gpr[ra], i10); goto advance; /* cgti */
        case 0x4D: ctx->gpr[rt] = spu_cgthi(ctx->gpr[ra], i10); goto advance; /* cgthi */
        case 0x4E: ctx->gpr[rt] = spu_cgtbi(ctx->gpr[ra], i10); goto advance; /* cgtbi */
        case 0x5C: ctx->gpr[rt] = spu_clgti(ctx->gpr[ra], i10); goto advance; /* clgti */
        case 0x5D: ctx->gpr[rt] = spu_clgthi(ctx->gpr[ra], i10); goto advance; /* clgthi */
        case 0x5E: ctx->gpr[rt] = spu_clgtbi(ctx->gpr[ra], i10); goto advance; /* clgtbi */
        case 0x74: ctx->gpr[rt] = spu_mpyi(ctx->gpr[ra], i10); goto advance; /* mpyi */
        case 0x75: ctx->gpr[rt] = spu_mpyui(ctx->gpr[ra], i10); goto advance; /* mpyui */
        case 0x7C: ctx->gpr[rt] = spu_ceqi(ctx->gpr[ra], i10); goto advance; /* ceqi */
        case 0x7D: ctx->gpr[rt] = spu_ceqhi(ctx->gpr[ra], i10); goto advance; /* ceqhi */
        case 0x7E: ctx->gpr[rt] = spu_ceqbi(ctx->gpr[ra], i10); goto advance; /* ceqbi */
        default: break; }
        /* RI8 (op10) */
        switch (op10) {
        case 0x1D8: ctx->gpr[rt] = spu_cflts(ctx->gpr[ra], i8); goto advance; /* cflts */
        case 0x1D9: ctx->gpr[rt] = spu_cfltu(ctx->gpr[ra], i8); goto advance; /* cfltu */
        case 0x1DA: ctx->gpr[rt] = spu_csflt(ctx->gpr[ra], i8); goto advance; /* csflt */
        case 0x1DB: ctx->gpr[rt] = spu_cuflt(ctx->gpr[ra], i8); goto advance; /* cuflt */
        default: break; }
        /* RR (op11) */
        switch (op11) {
        case 0x0: ctx->status = SPU_STATUS_STOPPED_BY_STOP; goto advance; /* stop */
        case 0x1: /* nop */; goto advance; /* lnop */
        case 0x2: /* sync */; goto advance; /* sync */
        case 0x3: /* sync */; goto advance; /* dsync */
        case 0xC: ctx->gpr[rt] = spu_mfspr(ctx->gpr[ra]); goto advance; /* mfspr */
        case 0x40: ctx->gpr[rt] = spu_sf(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* sf */
        case 0x41: ctx->gpr[rt] = spu_or(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* or */
        case 0x42: ctx->gpr[rt] = spu_bg(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* bg */
        case 0x48: ctx->gpr[rt] = spu_sfh(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* sfh */
        case 0x49: ctx->gpr[rt] = spu_nor(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* nor */
        case 0x53: /* TODO spu: absdb $r3, $r5, $r7 */; goto advance; /* absdb */
        case 0x58: ctx->gpr[rt] = spu_rot(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* rot */
        case 0x59: ctx->gpr[rt] = spu_rotm(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* rotm */
        case 0x5A: ctx->gpr[rt] = spu_rotma(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* rotma */
        case 0x5B: ctx->gpr[rt] = spu_shl(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* shl */
        case 0x5C: ctx->gpr[rt] = spu_roth(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* roth */
        case 0x5D: ctx->gpr[rt] = spu_rothm(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* rothm */
        case 0x5E: /* TODO spu: rotmah $r3, $r5, $r7 */; goto advance; /* rotmah */
        case 0x5F: ctx->gpr[rt] = spu_shlh(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* shlh */
        case 0xC0: ctx->gpr[rt] = spu_a(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* a */
        case 0xC1: ctx->gpr[rt] = spu_and(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* and */
        case 0xC2: ctx->gpr[rt] = spu_cg(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* cg */
        case 0xC8: ctx->gpr[rt] = spu_ah(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* ah */
        case 0xC9: ctx->gpr[rt] = spu_nand(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* nand */
        case 0xD3: /* TODO spu: avgb $r3, $r5, $r7 */; goto advance; /* avgb */
        case 0x10C: /* TODO spu: mtspr $r3, $r5, $r7 */; goto advance; /* mtspr */
        case 0x140: ctx->status = SPU_STATUS_STOPPED_BY_STOP; goto advance; /* stopd */
        case 0x144: spu_ls_write128(ctx, ctx->gpr[ra]._u32[0] + ctx->gpr[rb]._u32[0], ctx->gpr[rt]); goto advance; /* stqx */
        case 0x1AC: /* branch hint (ignored) */; goto advance; /* hbr */
        case 0x1B0: ctx->gpr[rt] = spu_gb(ctx->gpr[ra]); goto advance; /* gb */
        case 0x1B1: ctx->gpr[rt] = spu_gbh(ctx->gpr[ra]); goto advance; /* gbh */
        case 0x1B2: ctx->gpr[rt] = spu_gbb(ctx->gpr[ra]); goto advance; /* gbb */
        case 0x1B4: ctx->gpr[rt] = spu_fsm(ctx->gpr[ra]); goto advance; /* fsm */
        case 0x1B5: ctx->gpr[rt] = spu_fsmh(ctx->gpr[ra]); goto advance; /* fsmh */
        case 0x1B6: ctx->gpr[rt] = spu_fsmb(ctx->gpr[ra]); goto advance; /* fsmb */
        case 0x1B8: ctx->gpr[rt] = spu_frest(ctx->gpr[ra]); goto advance; /* frest */
        case 0x1B9: ctx->gpr[rt] = spu_frsqest(ctx->gpr[ra]); goto advance; /* frsqest */
        case 0x1C4: ctx->gpr[rt] = spu_ls_read128(ctx, ctx->gpr[ra]._u32[0] + ctx->gpr[rb]._u32[0]); goto advance; /* lqx */
        case 0x1CC: ctx->gpr[rt] = spu_rotqbybi(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* rotqbybi */
        case 0x1CD: ctx->gpr[rt] = spu_rotqmbybi(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* rotqmbybi */
        case 0x1CF: ctx->gpr[rt] = spu_shlqbybi(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* shlqbybi */
        case 0x1D4: ctx->gpr[rt] = spu_cbx(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* cbx */
        case 0x1D5: ctx->gpr[rt] = spu_chx(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* chx */
        case 0x1D6: ctx->gpr[rt] = spu_cwx(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* cwx */
        case 0x1D7: ctx->gpr[rt] = spu_cdx(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* cdx */
        case 0x1D8: ctx->gpr[rt] = spu_rotqbi(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* rotqbi */
        case 0x1D9: ctx->gpr[rt] = spu_rotqmbi(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* rotqmbi */
        case 0x1DB: ctx->gpr[rt] = spu_shlqbi(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* shlqbi */
        case 0x1DC: ctx->gpr[rt] = spu_rotqby(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* rotqby */
        case 0x1DD: ctx->gpr[rt] = spu_rotqmby(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* rotqmby */
        case 0x1DF: ctx->gpr[rt] = spu_shlqby(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* shlqby */
        case 0x1F0: ctx->gpr[rt] = spu_orx(ctx->gpr[ra]); goto advance; /* orx */
        case 0x201: /* nop */; goto advance; /* nop */
        case 0x240: ctx->gpr[rt] = spu_cgt(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* cgt */
        case 0x241: ctx->gpr[rt] = spu_xor(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* xor */
        case 0x248: ctx->gpr[rt] = spu_cgth(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* cgth */
        case 0x249: /* TODO spu: eqv $r3, $r5, $r7 */; goto advance; /* eqv */
        case 0x250: ctx->gpr[rt] = spu_cgtb(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* cgtb */
        case 0x253: ctx->gpr[rt] = spu_sumb(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* sumb */
        case 0x258: /* hgt: halt-on-condition (no-op in recomp) */; goto advance; /* hgt */
        case 0x2A5: ctx->gpr[rt] = spu_clz(ctx->gpr[ra]); goto advance; /* clz */
        case 0x2A6: ctx->gpr[rt] = spu_xswd(ctx->gpr[ra]); goto advance; /* xswd */
        case 0x2AE: ctx->gpr[rt] = spu_xshw(ctx->gpr[ra]); goto advance; /* xshw */
        case 0x2B4: ctx->gpr[rt] = spu_cntb(ctx->gpr[ra]); goto advance; /* cntb */
        case 0x2B6: ctx->gpr[rt] = spu_xsbh(ctx->gpr[ra]); goto advance; /* xsbh */
        case 0x2C0: ctx->gpr[rt] = spu_clgt(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* clgt */
        case 0x2C1: ctx->gpr[rt] = spu_andc(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* andc */
        case 0x2C2: ctx->gpr[rt] = spu_fcgt(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* fcgt */
        case 0x2C3: /* TODO spu: dfcgt $r3, $r5, $r7 */; goto advance; /* dfcgt */
        case 0x2C4: ctx->gpr[rt] = spu_fa(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* fa */
        case 0x2C5: ctx->gpr[rt] = spu_fs(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* fs */
        case 0x2C6: ctx->gpr[rt] = spu_fm(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* fm */
        case 0x2C8: ctx->gpr[rt] = spu_clgth(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* clgth */
        case 0x2C9: ctx->gpr[rt] = spu_orc(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* orc */
        case 0x2CA: ctx->gpr[rt] = spu_fcmgt(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* fcmgt */
        case 0x2CB: /* TODO spu: dfcmgt $r3, $r5, $r7 */; goto advance; /* dfcmgt */
        case 0x2CC: ctx->gpr[rt] = spu_dfa(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* dfa */
        case 0x2CD: ctx->gpr[rt] = spu_dfs(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* dfs */
        case 0x2CE: ctx->gpr[rt] = spu_dfm(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* dfm */
        case 0x2D0: ctx->gpr[rt] = spu_clgtb(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* clgtb */
        case 0x2D8: /* hlgt: halt-on-condition (no-op in recomp) */; goto advance; /* hlgt */
        case 0x340: ctx->gpr[rt] = spu_addx(ctx->gpr[ra], ctx->gpr[rb], ctx->gpr[rt]); goto advance; /* addx */
        case 0x341: ctx->gpr[rt] = spu_sfx(ctx->gpr[ra], ctx->gpr[rb], ctx->gpr[rt]); goto advance; /* sfx */
        case 0x342: /* TODO spu: cgx $r3, $r5, $r7 */; goto advance; /* cgx */
        case 0x343: ctx->gpr[rt] = spu_bgx(ctx->gpr[ra], ctx->gpr[rb], ctx->gpr[rt]); goto advance; /* bgx */
        case 0x346: /* TODO spu: mpyhha $r3, $r5, $r7 */; goto advance; /* mpyhha */
        case 0x34E: /* TODO spu: mpyhhau $r3, $r5, $r7 */; goto advance; /* mpyhhau */
        case 0x35C: /* TODO spu: dfma $r3, $r5, $r7 */; goto advance; /* dfma */
        case 0x35D: /* TODO spu: dfms $r3, $r5, $r7 */; goto advance; /* dfms */
        case 0x35E: /* TODO spu: dfnms $r3, $r5, $r7 */; goto advance; /* dfnms */
        case 0x35F: /* TODO spu: dfnma $r3, $r5, $r7 */; goto advance; /* dfnma */
        case 0x398: ctx->gpr[rt] = spu_fscrrd(ctx->gpr[ra]); goto advance; /* fscrrd */
        case 0x3B8: ctx->gpr[rt] = spu_fesd(ctx->gpr[ra]); goto advance; /* fesd */
        case 0x3B9: ctx->gpr[rt] = spu_frds(ctx->gpr[ra]); goto advance; /* frds */
        case 0x3BA: /* fscrwr: FPSCR write (no FPSCR model, no-op) */; goto advance; /* fscrwr */
        case 0x3BF: /* TODO spu: dftsv $r3, $r5, $r7 */; goto advance; /* dftsv */
        case 0x3C0: ctx->gpr[rt] = spu_ceq(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* ceq */
        case 0x3C2: ctx->gpr[rt] = spu_fceq(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* fceq */
        case 0x3C3: /* TODO spu: dfceq $r3, $r5, $r7 */; goto advance; /* dfceq */
        case 0x3C4: ctx->gpr[rt] = spu_mpy(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* mpy */
        case 0x3C5: ctx->gpr[rt] = spu_mpyh(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* mpyh */
        case 0x3C6: ctx->gpr[rt] = spu_mpyhh(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* mpyhh */
        case 0x3C7: ctx->gpr[rt] = spu_mpys(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* mpys */
        case 0x3C8: ctx->gpr[rt] = spu_ceqh(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* ceqh */
        case 0x3CA: ctx->gpr[rt] = spu_fcmeq(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* fcmeq */
        case 0x3CB: ctx->gpr[rt] = spu_dfcmeq(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* dfcmeq */
        case 0x3CC: ctx->gpr[rt] = spu_mpyu(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* mpyu */
        case 0x3CE: /* TODO spu: mpyhhu $r3, $r5, $r7 */; goto advance; /* mpyhhu */
        case 0x3D0: ctx->gpr[rt] = spu_ceqb(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* ceqb */
        case 0x3D4: ctx->gpr[rt] = spu_fi(ctx->gpr[ra], ctx->gpr[rb]); goto advance; /* fi */
        case 0x3D8: /* heq: halt-on-condition (no-op in recomp) */; goto advance; /* heq */
        case 0x1A8: nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance; /* bi */
        case 0x1A9: ctx->gpr[rt] = spu_splat_u32(pc+4); nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance; /* bisl */
        case 0x128: if (ctx->gpr[rt]._u32[0]==0) nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance; /* biz */
        case 0x129: if (ctx->gpr[rt]._u32[0]!=0) nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance; /* binz */
        case 0x12A: if (ctx->gpr[rt]._u16[0]==0) nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance; /* bihz */
        case 0x12B: if (ctx->gpr[rt]._u16[0]!=0) nextpc = ctx->gpr[ra]._u32[0] & ~3u; goto advance; /* bihnz */
        case 0x1AA: return 0; /* iret non supporté: halt */ /* iret */
        default: break; }
        /* RI7 (op11) */
        switch (op11) {
        case 0x78: ctx->gpr[rt] = spu_roti(ctx->gpr[ra], i7u); goto advance; /* roti */
        case 0x79: ctx->gpr[rt] = spu_rotmi(ctx->gpr[ra], i7u); goto advance; /* rotmi */
        case 0x7A: ctx->gpr[rt] = spu_rotmai(ctx->gpr[ra], i7u); goto advance; /* rotmai */
        case 0x7B: ctx->gpr[rt] = spu_shli(ctx->gpr[ra], i7u); goto advance; /* shli */
        case 0x7C: ctx->gpr[rt] = spu_rothi(ctx->gpr[ra], i7u); goto advance; /* rothi */
        case 0x7D: ctx->gpr[rt] = spu_rothmi(ctx->gpr[ra], i7u); goto advance; /* rothmi */
        case 0x7E: ctx->gpr[rt] = spu_rotmahi(ctx->gpr[ra], i7u); goto advance; /* rotmahi */
        case 0x7F: ctx->gpr[rt] = spu_shlhi(ctx->gpr[ra], i7u); goto advance; /* shlhi */
        case 0x1F4: ctx->gpr[rt] = spu_cbd(ctx->gpr[ra], i7u); goto advance; /* cbd */
        case 0x1F5: ctx->gpr[rt] = spu_chd(ctx->gpr[ra], i7u); goto advance; /* chd */
        case 0x1F6: ctx->gpr[rt] = spu_cwd(ctx->gpr[ra], i7u); goto advance; /* cwd */
        case 0x1F7: ctx->gpr[rt] = spu_cdd(ctx->gpr[ra], i7u); goto advance; /* cdd */
        case 0x1F8: ctx->gpr[rt] = spu_rotqbii(ctx->gpr[ra], i7u); goto advance; /* rotqbii */
        case 0x1F9: ctx->gpr[rt] = spu_rotqmbii(ctx->gpr[ra], i7u); goto advance; /* rotqmbii */
        case 0x1FB: ctx->gpr[rt] = spu_shlqbii(ctx->gpr[ra], i7u); goto advance; /* shlqbii */
        case 0x1FC: ctx->gpr[rt] = spu_rotqbyi(ctx->gpr[ra], i7u); goto advance; /* rotqbyi */
        case 0x1FD: ctx->gpr[rt] = spu_rotqmbyi(ctx->gpr[ra], i7u); goto advance; /* rotqmbyi */
        case 0x1FF: ctx->gpr[rt] = spu_shlqbyi(ctx->gpr[ra], i7u); goto advance; /* shlqbyi */
        default: break; }
        {   /* aucun format n'a matché: opcode réellement non géré */
            static int _u = 0;
            if (_u < 48) { _u++;
                fprintf(stderr, "[spu-interp] NON GERE pc=0x%05X insn=0x%08X "
                    "op4=0x%X op7=0x%02X op8=0x%02X op9=0x%03X op10=0x%03X op11=0x%03X\n",
                    pc, insn, op4, op7, op8, op9, op10, op11);
            }
        }
        return -1;
    advance:
        ctx->pc = nextpc;
    }
    return 0; /* budget d'instructions épuisé */
}
