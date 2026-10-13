"""Self-check for the count of `shl` and `shr`.

x86 masks a shift count to 5 bits for 8-, 16- and 32-bit operands: `shr eax,
cl` with cl = 33 shifts by 1. `sar` has lifted that way for a while (see
test_lifter_sar_width.py); `shl` and `shr` passed the count through as
written, in the shift and in the carry they compute from it. In C that is
undefined as soon as the count reaches the width of the promoted operand, and
so is the left shift of a narrow operand that carries a bit into the sign of
the int it was promoted to (`al << 24` with al = 0x80).

The checks compile the lifter's own output and sweep it against the x86
answer, worked out from the masked count with types of the operand's width.
Carry is compared only where x86 defines it, for a masked count from 1 to
one below the operand's width; a masked count of 0 must leave it alone.

Run: py -3 -m unittest tools.recomp.test_lifter_shift_count
"""

import os
import shutil
import subprocess
import tempfile
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _shift(mnemonic, dst, cnt, needs_cf=True):
    """Lift one `shl`/`shr dst, cnt` and return its statements."""
    insn = Instruction(0, 2, mnemonic, "", "00")
    insn.operands = [dst, cnt]
    lifter = Lifter()
    lifter.needs_cf = needs_cf
    return lifter.lift_instruction(insn)


# Upstream's register and memory accessors, as in
# templates/runtime/recomp_types.h, and the variables the result snapshot
# that follows a shift writes to.
PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#define LO8(r)  ((uint8_t)((r) & 0xFF))
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))
static uint32_t _fa, _fb;
static int32_t _fas, _fbs;
static int _fk;
static uint8_t g_ram[64];
#define MEM8(a)  (*(volatile uint8_t *)(g_ram + (a)))
#define MEM16(a) (*(volatile uint16_t *)(g_ram + (a)))
#define W WIDTH
/* What x86 does, from the count it actually uses. */
static uint32_t want_value(uint32_t v, uint32_t c) {
    uint32_t n = c & 31u, mask = (W == 32) ? 0xFFFFFFFFu : ((1u << W) - 1u);
    if (n >= W) return 0;
    return (IS_SHR ? (v >> n) : (v << n)) & mask;
}
/* Carry is the last bit shifted out. -1: x86 leaves it undefined, or as it was. */
static int want_carry(uint32_t v, uint32_t c) {
    uint32_t n = c & 31u;
    if (n == 0 || n >= W) return -1;
    return (int)((IS_SHR ? (v >> (n - 1u)) : (v >> (W - n))) & 1u);
}
"""

# One sweep over values and counts. COUNT is how the count reaches the lifted
# statements: through cl, or not at all when the count is an immediate.
BODY = r"""
static const uint32_t VS[] = {0u, 1u, 2u, 0x55u, 0x7Fu, 0x80u, 0xAAu, 0xFFu,
    0x7FFFu, 0x8000u, 0xFFFFu, 0x40000000u, 0x7FFFFFFFu, 0x80000000u,
    0xDEADBEEFu, 0xFFFFFFFFu};
int main(void) {
    for (unsigned vi = 0; vi < sizeof(VS) / sizeof(VS[0]); vi++) {
        for (uint32_t c = CMIN; c <= CMAX; c++) {
            for (int cf_in = 0; cf_in <= 1; cf_in++) {
                const uint32_t mask = (W == 32) ? 0xFFFFFFFFu : ((1u << W) - 1u);
                const uint32_t v = VS[vi] & mask;
                uint32_t eax = 0xAAAAAA00u, ecx = 0xBBBBBB00u | c, esi = 0;
                int _cf = cf_in;
                (void)esi; (void)ecx;
                SEED;
                STMTS
                {
                    uint32_t got = (uint32_t)(GOT), want = want_value(v, c);
                    int wc = want_carry(v, c);
                    if (got != want) {
                        printf("FAIL value v=0x%X c=%u got=0x%X want=0x%X\n", v, c, got, want);
                        return 1;
                    }
                    if ((c & 31u) == 0 && _cf != cf_in) {
                        printf("FAIL carry changed by a count of 0: v=0x%X c=%u\n", v, c);
                        return 1;
                    }
                    if (wc >= 0 && _cf != wc) {
                        printf("FAIL carry v=0x%X c=%u got=%d want=%d\n", v, c, _cf, wc);
                        return 1;
                    }
                    if ((eax & KEEPMASK) != (0xAAAAAA00u & KEEPMASK)) {
                        printf("FAIL v=0x%X c=%u clobbered eax=0x%X\n", v, c, eax);
                        return 1;
                    }
                }
            }
        }
    }
    printf("OK\n");
    return 0;
}
"""

# dst operand, seed, read-back, width in bits, untouched-bits mask.
CASES = {
    "al": (Operand(type="reg", reg="al"),
           "eax = (eax & 0xFFFFFF00u) | v", "LO8(eax)", 8, "0xFFFFFF00u"),
    "ah": (Operand(type="reg", reg="ah"),
           "eax = (eax & 0xFFFF00FFu) | (v << 8)", "HI8(eax)", 8, "0xFFFF00FFu"),
    "ax": (Operand(type="reg", reg="ax"),
           "eax = (eax & 0xFFFF0000u) | v", "LO16(eax)", 16, "0xFFFF0000u"),
    "eax": (Operand(type="reg", reg="eax"),
            "eax = v", "eax", 32, "0u"),
    "byte [esi]": (Operand(type="mem", mem_base="esi", mem_size=1),
                   "MEM8(0) = (uint8_t)v", "MEM8(0)", 8, "0u"),
}

CL = Operand(type="reg", reg="cl")


def _find_cc():
    """A C compiler and how to call it: (path, is_msvc)."""
    for name in ("clang", "gcc", "cc"):
        if shutil.which(name):
            return shutil.which(name), False
    if shutil.which("cl"):
        return shutil.which("cl"), True
    return None, False


class ShiftCountTest(unittest.TestCase):
    def _run(self, name, mnemonic, stmts, cmin, cmax):
        """Compile and run one sweep. Returns (returncode, output)."""
        _, seed, got, width, keep = CASES[name]
        src = (PRELUDE + BODY)
        for key, val in (("SEED", seed), ("STMTS", "\n".join(stmts)),
                         ("GOT", got), ("WIDTH", str(width)),
                         ("IS_SHR", "1" if mnemonic == "shr" else "0"),
                         ("CMIN", str(cmin)), ("CMAX", str(cmax)),
                         ("KEEPMASK", keep)):
            src = src.replace(key, val)
        cc, msvc = _find_cc()
        with tempfile.TemporaryDirectory() as tmp:
            c = os.path.join(tmp, "t.c")
            with open(c, "w") as f:
                f.write(src)
            exe = os.path.join(tmp, "t.exe")
            cmd = ([cc, "/nologo", "/w", "/O2", c, "/Fe:" + exe, "/Fo:" + tmp + os.sep]
                   if msvc else [cc, "-w", "-O2", c, "-o", exe])
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0,
                             (r.stdout + r.stderr)[-1500:] + "\n" + src)
            r = subprocess.run([exe], capture_output=True, text=True)
            return r.returncode, r.stdout + r.stderr

    def test_count_is_masked_in_the_shift_and_in_the_carry(self):
        """x86 masks the count; C leaves a shift of 33 undefined.

        Asserted on the emitted text, as for `sar`: running the unmasked form
        proves nothing either way, since the common host lowering happens to
        mask a count it was not asked to.
        """
        for mnemonic in ("shl", "shr"):
            for name, (dst, *_rest) in CASES.items():
                for cnt in (CL, Operand(type="imm", imm=33)):
                    with self.subTest(insn=mnemonic, operand=name, count=cnt.type):
                        carry, write = _shift(mnemonic, dst, cnt)[:2]
                        self.assertIn("_cf =", carry)
                        self.assertIn("& 31", carry)
                        self.assertIn("& 31", write)
                        self.assertNotIn("_cf", write)

    def test_register_count_matches_x86_at_every_width(self):
        """`shl`/`shr dst, cl` for every count a byte register can ask for
        that matters: 0 to 63 covers in range, at the width, past it, and
        past 31 where the mask brings it round again."""
        if not _find_cc()[0]:
            self.skipTest("no C compiler on PATH")
        for mnemonic in ("shl", "shr"):
            for name, (dst, *_rest) in CASES.items():
                with self.subTest(insn=mnemonic, operand=name):
                    stmts = _shift(mnemonic, dst, CL)
                    rc, out = self._run(name, mnemonic, stmts, 0, 63)
                    self.assertEqual(rc, 0, f"{out}\n  " + "\n  ".join(stmts))
                    self.assertTrue(out.startswith("OK"), out)

    def test_constant_count_matches_x86(self):
        """A constant count, which the host compiler folds: 33 is 1, 32 is 0
        and leaves everything alone, 0x32 on a byte is 18 and empties it."""
        if not _find_cc()[0]:
            self.skipTest("no C compiler on PATH")
        for mnemonic in ("shl", "shr"):
            for name in ("al", "ax", "eax"):
                for imm in (1, 7, 8, 31, 32, 33, 0x32, 0xCA):
                    with self.subTest(insn=mnemonic, operand=name, count=imm):
                        stmts = _shift(mnemonic, CASES[name][0],
                                       Operand(type="imm", imm=imm))
                        rc, out = self._run(name, mnemonic, stmts, imm, imm)
                        self.assertEqual(rc, 0, f"{out}\n  " + "\n  ".join(stmts))
                        self.assertTrue(out.startswith("OK"), out)

    def test_no_carry_statement_when_nothing_reads_carry(self):
        """A function with no adc/sbb pays nothing for carry; the count is
        masked all the same."""
        for mnemonic in ("shl", "shr"):
            with self.subTest(insn=mnemonic):
                out = "\n".join(_shift(mnemonic, CASES["eax"][0], CL, needs_cf=False))
                self.assertNotIn("_cf", out)
                self.assertIn("& 31", out)


if __name__ == "__main__":
    unittest.main()
