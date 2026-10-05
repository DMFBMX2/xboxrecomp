"""lahf reads CF, so a function whose only carry reader is a lahf declares it.

LAHF is lifted from the tracked flag setter, one bit at a time, through the
conditions a jcc would use; bit 0 is CF, asked for as "jb". After an inc or a
dec that condition is `_cf` -- they leave CF alone, so it is whatever an
earlier instruction left -- and _function_needs_cf counted only jcc, setcc and
cmovcc as readers. A function with `dec; lahf` and no other carry consumer
named a `_cf` it never declared, and the generated C did not compile. Dave
Mirra Freestyle BMX 2 has five.
"""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block
from .translator import FunctionTranslator


def _insns(setter):
    first = Instruction(0, 1, setter, "edi", "4f")
    first.operands = [Operand(type="reg", reg="edi")]
    lahf = Instruction(1, 1, "lahf", "", "9f")
    lahf.operands = []
    return [first, lahf]


class LahfCarryTest(unittest.TestCase):
    def test_lahf_after_dec_reads_the_carry_variable(self):
        lifter = Lifter()
        lifter.needs_cf = True
        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=_insns("dec")))
        self.assertIn("_cf", "\n".join(lifted))

    def test_such_a_function_declares_it(self):
        for setter in ("inc", "dec"):
            with self.subTest(setter=setter):
                self.assertTrue(
                    FunctionTranslator._function_needs_cf(_insns(setter)))


if __name__ == "__main__":
    unittest.main()
