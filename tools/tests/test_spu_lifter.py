import os
import sys
import unittest


TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS_DIR)

from spu_disasm import SPUInstruction, spu_decode
from spu_lifter import SPULifter


class LinkRegisterTests(unittest.TestCase):
    def lift(self, insns, bounds):
        lifter = SPULifter()
        lifter.prepare_control_flow(insns, bounds)
        for start, end in bounds:
            lifter.lift_function(insns, start, end)
        return lifter.emit_source()

    def test_nonstandard_brsl_link_register_becomes_return(self):
        insns = [
            SPUInstruction(0x100, 78, "brsl", "0x200"),
            SPUInstruction(0x200, 0, "bi", "$r78"),
        ]
        source = self.lift(insns, [(0x100, 0x104), (0x200, 0x204)])
        body = source.split("void spu_func_00000200", 1)[1]
        self.assertIn("ctx->gpr[78]._u32[0] == 0x104u", body)
        self.assertIn("ctx->pc = ctx->gpr[78]._u32[0]", body)

    def test_link_register_propagates_through_tail_branch(self):
        insns = [
            SPUInstruction(0x100, 78, "brsl", "0x200"),
            SPUInstruction(0x200, 0, "br", "0x300"),
            SPUInstruction(0x300, 0, "bi", "$r78"),
        ]
        source = self.lift(
            insns, [(0x100, 0x104), (0x200, 0x204), (0x300, 0x304)])
        body = source.split("void spu_func_00000300", 1)[1]
        self.assertIn("ctx->gpr[78]._u32[0] == 0x104u", body)
        self.assertIn("ctx->pc = ctx->gpr[78]._u32[0]", body)

    def test_mixed_call_and_tail_entry_dispatches_non_call_return(self):
        insns = [
            SPUInstruction(0x100, 6, "brsl", "0x200"),
            SPUInstruction(0x180, 0, "br", "0x200"),
            SPUInstruction(0x200, 0, "bi", "$r6"),
        ]
        source = self.lift(
            insns, [(0x100, 0x104), (0x180, 0x184), (0x200, 0x204)])
        body = source.split("void spu_func_00000200", 1)[1]
        self.assertIn("ctx->gpr[6]._u32[0] == 0x104u", body)
        self.assertIn("ctx->pc = ctx->gpr[6]._u32[0]", body)

    def test_unrelated_indirect_branch_stays_indirect(self):
        insns = [SPUInstruction(0x200, 0, "bi", "$r78")]
        source = self.lift(insns, [(0x200, 0x204)])
        self.assertIn("ctx->pc = ctx->gpr[78]._u32[0]", source)

    def test_halfword_branch_uses_runtime_preferred_halfword(self):
        lifter = SPULifter()
        self.assertEqual(lifter._cond("brhz", "3"),
                         "ctx->gpr[3]._u16[0] == 0")
        self.assertEqual(lifter._cond("ihnz", "7"),
                         "ctx->gpr[7]._u16[0] != 0")

    def test_manual_r0_link_branch_resumes_at_fallthrough(self):
        insns = [
            SPUInstruction(0x100, 0, "ila", "$r0, 0x10C"),
            SPUInstruction(0x104, 2, "cgti", "$r2, $r2, -1"),
            SPUInstruction(0x108, 0, "brnz", "$r2, 0x200"),
            SPUInstruction(0x10C, 3, "il", "$r3, 1"),
            SPUInstruction(0x200, 0, "bi", "$r0"),
        ]
        source = self.lift(insns, [(0x100, 0x110), (0x200, 0x204)])
        branch = source.split("ctx->pc = 0x200", 1)[1].split("}", 1)[0]
        self.assertIn("spu_func_00000200(ctx);", branch)
        self.assertNotIn("return;", branch)

    def test_rrr_decode_uses_rt4_destination_and_low_rc(self):
        cases = {
            0x8: "selb", 0xB: "shufb", 0xC: "mpya",
            0xD: "fnms", 0xE: "fma", 0xF: "fms",
        }
        for opcode, mnemonic in cases.items():
            raw = (opcode << 28) | (6 << 21) | (4 << 14) | (5 << 7) | 3
            insn = spu_decode(raw, 0x100)
            self.assertEqual(insn.mnemonic, mnemonic)
            self.assertEqual(insn.operands, "$r6, $r5, $r4, $r3")

    def test_rrr_lift_writes_rt4_and_uses_rc_control(self):
        insn = spu_decode(0xB0C10303, 0x1E78)
        source = self.lift([insn], [(0x1E78, 0x1E7C)])
        self.assertIn(
            "ctx->gpr[6] = spu_shufb(ctx->gpr[6], ctx->gpr[4], ctx->gpr[3]);",
            source)

    def test_real_policy_selb_opcode(self):
        insn = spu_decode(0x80608180, 0x2DE4)
        self.assertEqual(insn.mnemonic, "selb")
        self.assertEqual(insn.operands, "$r3, $r3, $r2, $r0")
        source = self.lift([insn], [(0x2DE4, 0x2DE8)])
        self.assertIn(
            "ctx->gpr[3] = spu_selb(ctx->gpr[3], ctx->gpr[2], ctx->gpr[0]);",
            source)

    def test_quadword_ls_targets_use_word_scale_and_quadword_mask(self):
        # This exact policy store targets LS 0x0D80. Scaling it by 16 instead
        # overwrites executable policy code at LS 0x3600.
        stqa = spu_decode(0x2081B005, 0x26FC)
        self.assertEqual(stqa.mnemonic, "stqa")
        self.assertEqual(stqa.operands, "$r5, 0xD80")

        stqr_raw = (0x47 << 23) | (1 << 7) | 5
        stqr = spu_decode(stqr_raw, 0x1004)
        self.assertEqual(stqr.mnemonic, "stqr")
        self.assertEqual(stqr.operands, "$r5, 0x1000")

    def test_screen_space_culling_orbi_sequence(self):
        cases = {
            0x06010108: ("$r8, $r2, 4", "ctx->gpr[8] = spu_orbi(ctx->gpr[2], 4);"),
            0x0602010A: ("$r10, $r2, 8", "ctx->gpr[10] = spu_orbi(ctx->gpr[2], 8);"),
            0x0603010C: ("$r12, $r2, 12", "ctx->gpr[12] = spu_orbi(ctx->gpr[2], 12);"),
        }
        for raw, (operands, lifted) in cases.items():
            insn = spu_decode(raw, 0x100)
            self.assertEqual(insn.mnemonic, "orbi")
            self.assertEqual(insn.operands, operands)
            self.assertIn(lifted, self.lift([insn], [(0x100, 0x104)]))

    def test_brsl_to_next_only_runs_fallthrough_once(self):
        insns = [
            SPUInstruction(0x100, 126, "brsl", "0x104"),
            SPUInstruction(0x104, 0, "bi", "$r0"),
        ]
        source = self.lift(insns, [(0x100, 0x104), (0x104, 0x108)])
        body = source.split("void spu_func_00000100", 1)[1].split(
            "void spu_func_00000104", 1)[0]
        self.assertIn("ctx->gpr[126] = spu_splat_u32(0x104);", body)
        self.assertIn("brsl-to-next link setup", body)
        self.assertEqual(body.count("spu_func_00000104(ctx);"), 1)

    def test_frest_uses_reciprocal_estimate_helper(self):
        insn = SPUInstruction(0x100, 0, "frest", "$r60, $r59, $r0")
        source = self.lift([insn], [(0x100, 0x104)])
        self.assertIn("ctx->gpr[60] = spu_frest(ctx->gpr[59]);", source)

    def test_link_spilled_to_ls_and_restored_in_other_register_returns(self):
        insns = [
            SPUInstruction(0x100, 0, "brsl", "0x200"),
            SPUInstruction(0x200, 0, "stqa", "$r0, 0x300"),
            SPUInstruction(0x204, 5, "lqa", "$r5, 0x300"),
            SPUInstruction(0x208, 0, "bi", "$r5"),
        ]
        source = self.lift(insns, [(0x100, 0x104), (0x200, 0x20C)])
        body = source.split("void spu_func_00000200", 1)[1]
        self.assertIn("ctx->gpr[5]._u32[0] == 0x104u", body)
        self.assertIn("return;", body)


    def test_function_trace_emits_one_entry_pc(self):
        insns = [SPUInstruction(0x100, 3, "il", "$r3, 1")]
        lifter = SPULifter(trace_functions=True)
        lifter.prepare_control_flow(insns, [(0x100, 0x104)])
        lifter.lift_function(insns, 0x100, 0x104)
        source = lifter.emit_source()
        self.assertEqual(source.count("spu_trace_pc(ctx, 0x100);"), 1)


if __name__ == "__main__":
    unittest.main()
