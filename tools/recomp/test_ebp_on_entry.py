"""Translator: a chunk that reads ebp before writing it inherits its frame."""
import os
import sys
import unittest
from types import SimpleNamespace as I

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.translator import FunctionTranslator  # noqa: E402

reads = FunctionTranslator._reads_ebp_on_entry


class ReadsEbpOnEntryTest(unittest.TestCase):
    def test_area51_tail_chunk(self):
        # 0x000EACB6: the out-of-line tail of sub_000EAC30.
        self.assertTrue(reads([I(mnemonic="push", op_str="edi"),
                               I(mnemonic="mov", op_str="edx, 0x37cff0"),
                               I(mnemonic="mov", op_str="edi, ebp")]))

    def test_memory_operand_through_ebp_is_a_read(self):
        self.assertTrue(reads([I(mnemonic="mov", op_str="eax, dword ptr [ebp + 8]")]))

    def test_frame_prologue_is_not(self):
        self.assertFalse(reads([I(mnemonic="push", op_str="ebp"),
                                I(mnemonic="mov", op_str="ebp, esp"),
                                I(mnemonic="mov", op_str="eax, dword ptr [ebp + 8]")]))

    def test_load_from_the_stack_is_a_write(self):
        self.assertFalse(reads([I(mnemonic="sub", op_str="esp, 0x7c"),
                                I(mnemonic="mov", op_str="ebp, dword ptr [esp + 0x88]"),
                                I(mnemonic="mov", op_str="eax, dword ptr [ebp + 4]")]))

    def test_self_relative_load_reads(self):
        self.assertTrue(reads([I(mnemonic="mov", op_str="ebp, dword ptr [ebp + 4]")]))

    def test_no_ebp_before_a_branch_is_not(self):
        self.assertFalse(reads([I(mnemonic="xor", op_str="eax, eax"),
                                I(mnemonic="ret", op_str=""),
                                I(mnemonic="mov", op_str="eax, ebp")]))


if __name__ == "__main__":
    unittest.main()
