"""MXCSR is guest state, and the SSE integer conversions round by it.

STMXCSR and LDMXCSR fell to RECOMP_UNIMPL, so a store of MXCSR wrote nothing.
Jet Set Radio Future runs one every frame: a routine stores MXCSR, tests the
rounding-control bits and returns E_UNEXPECTED unless they say "nearest" --
with nothing stored, the answer was whatever that memory happened to hold.

CVTSS2SI/CVTSD2SI were lifted as a C cast, which truncates (and is undefined
out of range), where the hardware rounds by MXCSR.RC -- nearest-even unless
the title changed it -- and answers out-of-range values with the integer
indefinite. Only the CVTT forms truncate.

The emitted text is checked here, and the helper is compiled and run: a text
check cannot tell a correct nearest-even from one that rounds halves up.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction, Operand
from .lifter import Lifter, _EFLAGS_PRESERVE

_ROOT = Path(__file__).resolve().parents[2]


def _insn(mnemonic, op_str, operands):
    instruction = Instruction(0, 4, mnemonic, op_str, "")
    instruction.operands = operands
    return instruction


class MxcsrLiftTest(unittest.TestCase):
    def test_store_writes_the_guest_mxcsr(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("stmxcsr", "dword ptr [0x265150]",
                      [Operand(type="mem", mem_disp=0x265150, mem_size=4)])),
            ["MEM32(0x265150) = g_mxcsr; /* stmxcsr dword ptr [0x265150] */"],
        )

    def test_load_reads_the_guest_mxcsr(self):
        self.assertEqual(
            Lifter().lift_instruction(
                _insn("ldmxcsr", "dword ptr [esp + 4]",
                      [Operand(type="mem", mem_base="esp", mem_disp=4, mem_size=4)])),
            ["g_mxcsr = MEM32(esp + 4); /* ldmxcsr dword ptr [esp + 4] */"],
        )

    def test_neither_changes_eflags(self):
        self.assertIn("stmxcsr", _EFLAGS_PRESERVE)
        self.assertIn("ldmxcsr", _EFLAGS_PRESERVE)

    def test_runtime_declares_the_state(self):
        runtime_types = (_ROOT / "templates" / "runtime" / "recomp_types.h").read_text(encoding="utf-8")
        main = (_ROOT / "templates" / "new-game" / "src" / "main.c").read_text(encoding="utf-8")
        layout = (_ROOT / "src" / "kernel" / "xbox_memory_layout.c").read_text(encoding="utf-8")
        self.assertIn("extern RECOMP_TLS uint32_t g_mxcsr;", runtime_types)
        self.assertIn("extern RECOMP_TLS uint32_t g_mxcsr;", main)
        self.assertIn("RECOMP_TLS uint32_t g_mxcsr = 0x1F80u;", layout)


class SseConversionLiftTest(unittest.TestCase):
    def _lift(self, mnemonic, source):
        return Lifter().lift_instruction(
            _insn(mnemonic, "eax, " + source.reg if source.type == "reg" else "eax, [ecx]",
                  [Operand(type="reg", reg="eax"), source]))

    def test_rounding_forms_follow_mxcsr(self):
        out = "\n".join(self._lift("cvtss2si", Operand(type="reg", reg="xmm1")))
        self.assertIn("recomp_cvt_si32((double)xmm1.f[0], g_mxcsr, 0)", out)
        out = "\n".join(self._lift("cvtsd2si", Operand(type="reg", reg="xmm2")))
        self.assertIn("recomp_cvt_si32(xmm2.d[0], g_mxcsr, 0)", out)

    def test_truncating_forms_truncate(self):
        out = "\n".join(self._lift("cvttss2si", Operand(type="mem", mem_base="ecx", mem_size=4)))
        self.assertIn("recomp_cvt_si32((double)MEMF(ecx), g_mxcsr, 1)", out)
        out = "\n".join(self._lift("cvttsd2si", Operand(type="mem", mem_base="ecx", mem_size=8)))
        self.assertIn("recomp_cvt_si32(MEMD(ecx), g_mxcsr, 1)", out)

    def test_no_bare_cast_remains(self):
        for mnemonic in ("cvtss2si", "cvttss2si", "cvtsd2si", "cvttsd2si"):
            with self.subTest(mnemonic=mnemonic):
                out = "\n".join(self._lift(mnemonic, Operand(type="reg", reg="xmm0")))
                self.assertNotIn("(int32_t)xmm0", out)


class SseConversionRuntimeTest(unittest.TestCase):
    def test_conversions_round_like_the_hardware(self):
        cc = next(filter(None, map(shutil.which, ("cc", "gcc", "clang"))), None)
        if not cc:
            self.skipTest("no C compiler available")
        source = r'''
#include <stdint.h>
#include <math.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;

#define NEAR 0x1F80u   /* the reset value: RC=00 */
#define DOWN 0x3F80u   /* RC=01 */
#define UP   0x5F80u   /* RC=10 */
#define CHOP 0x7F80u   /* RC=11 */

int main(void) {
    /* Nearest-even, not truncation: the defect. */
    if (recomp_cvt_si32(2.7, NEAR, 0) != 3) return __LINE__;
    if (recomp_cvt_si32(-2.7, NEAR, 0) != -3) return __LINE__;
    if (recomp_cvt_si32(2.5, NEAR, 0) != 2) return __LINE__;
    if (recomp_cvt_si32(3.5, NEAR, 0) != 4) return __LINE__;
    /* The other rounding controls. */
    if (recomp_cvt_si32(2.7, DOWN, 0) != 2) return __LINE__;
    if (recomp_cvt_si32(-2.2, DOWN, 0) != -3) return __LINE__;
    if (recomp_cvt_si32(2.2, UP, 0) != 3) return __LINE__;
    if (recomp_cvt_si32(-2.7, CHOP, 0) != -2) return __LINE__;
    /* The truncating form ignores RC. */
    if (recomp_cvt_si32(2.7, NEAR, 1) != 2) return __LINE__;
    if (recomp_cvt_si32(-2.7, UP, 1) != -2) return __LINE__;
    /* The integer indefinite. */
    if (recomp_cvt_si32((double)NAN, NEAR, 0) != INT32_MIN) return __LINE__;
    if (recomp_cvt_si32((double)INFINITY, NEAR, 1) != INT32_MIN) return __LINE__;
    if (recomp_cvt_si32(3e9, NEAR, 0) != INT32_MIN) return __LINE__;
    if (recomp_cvt_si32(-3e9, NEAR, 1) != INT32_MIN) return __LINE__;
    /* The ends of the range still convert. */
    if (recomp_cvt_si32(2147483647.0, NEAR, 0) != INT32_MAX) return __LINE__;
    if (recomp_cvt_si32(-2147483648.0, NEAR, 1) != INT32_MIN) return __LINE__;
    if (recomp_cvt_si32(2147483647.4, NEAR, 0) != INT32_MAX) return __LINE__;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="mxcsr-runtime-") as tmp:
            tmp = Path(tmp)
            src, exe = tmp / "test.c", tmp / "test"
            src.write_text(source, encoding="utf-8")
            built = subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(_ROOT / "templates" / "runtime"),
                 str(src), "-o", str(exe)]
                    + ([] if os.name == "nt" else ["-lm"]),
                capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(0, ran.returncode,
                             f"check on line {ran.returncode} failed")


if __name__ == "__main__":
    unittest.main()
