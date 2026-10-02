"""A jcc into the middle of another function becomes an alias entry."""
import os
import sys
import unittest
from types import SimpleNamespace as I

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402


def _detector(insns):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = I(instructions={i.address: i for i in insns})
    det._alias_entries, det._candidates, det.functions = {}, {}, {}
    return det


class CondBranchIntoNeighbourTest(unittest.TestCase):
    # Area 51: sub_000EAC30 (to 0x000EACB6) does "jne 0xec2d2" into the
    # middle of sub_000EACB6 (to 0x000EC2DF).
    bodies = [(0x000EAC30, 0x000EACB6), (0x000EACB6, 0x000EC2DF)]
    starts = [b[0] for b in bodies]

    def test_jcc_into_a_neighbour_is_an_alias(self):
        jne = I(address=0x000EAC4A, is_cond_jump=True, jump_target=0x000EC2D2)
        tgt = I(address=0x000EC2D2, is_cond_jump=False, jump_target=None)
        det = _detector([jne, tgt])
        self.assertTrue(det._pass_cond_branch_orphans(self.bodies, self.starts))
        self.assertEqual(det._alias_entries, {0x000EC2D2: 0x000EC2DF})

    def test_jcc_to_a_neighbours_start_adds_nothing(self):
        jne = I(address=0x000EAC6E, is_cond_jump=True, jump_target=0x000EACB6)
        det = _detector([jne])
        det.functions = {0x000EACB6: object()}
        self.assertFalse(det._pass_cond_branch_orphans(self.bodies, self.starts))
        self.assertEqual(det._alias_entries, {})

    def test_target_off_an_instruction_boundary_is_ignored(self):
        jne = I(address=0x000EAC4A, is_cond_jump=True, jump_target=0x000EC2D3)
        det = _detector([jne])
        self.assertFalse(det._pass_cond_branch_orphans(self.bodies, self.starts))


if __name__ == "__main__":
    unittest.main()
