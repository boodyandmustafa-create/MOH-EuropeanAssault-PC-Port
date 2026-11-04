"""
Self-check for the flags of xadd / lock xadd.

Run: py -3 tools/recomp/test_xadd_flags.py

Regression guard for the bug where "lock xadd" was listed among the
instructions with undefined flags. A `jne` right after it -- InterlockedDecrement
followed by the "destroy when the count reached zero" test that every COM
Release compiles to -- then fell to the `_flags` fallback, which nothing
assigns, so the branch was never taken and Release destroyed the object
whatever the count was. Jet Set Radio Future freed the same object twice that
way as soon as its Direct3D device existed, corrupting the heap.

The lowering now publishes the sum into _fa/_fas next to the atomic, and the
condition reads that snapshot rather than the destination (which another
thread may have changed) or the source (which xadd overwrites with the old
value).
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import (  # noqa: E402
    Lifter,
    _FLAGS_UNDEFINED,
    _RESULT_SNAPSHOT_SETTERS,
    _make_condition,
)


class _Op:
    def __init__(self, text, type="reg", reg=None, size=4, mem_size=None, imm=0,
                 mem_base=None, mem_index=None, mem_scale=1, mem_disp=0, segment=None):
        self.text = text
        self.type = type
        self.reg = reg
        self.size = size
        self.mem_size = mem_size
        self.imm = imm
        self.mem_base = mem_base
        self.mem_index = mem_index
        self.mem_scale = mem_scale
        self.mem_disp = mem_disp
        self.segment = segment


class _Insn:
    def __init__(self, mnemonic, operands):
        self.mnemonic = mnemonic
        self.operands = operands
        self.address = 0
        self.op_str = ""
        self.is_cond_jump = False
        self.is_jump = False
        self.is_call = False
        self.is_ret = False
        self.is_branch = False
        self.jump_target = None
        self.jump_table = None


def _lift(mnemonic, ops):
    lifter = Lifter()
    return "\n".join(lifter.lift_instruction(_Insn(mnemonic, ops)))


REFCOUNT = _Op("dword ptr [ecx + 8]", type="mem", mem_base="ecx", mem_disp=8, mem_size=4)
EDX = _Op("edx", reg="edx")


def test_lock_xadd_is_not_undefined():
    assert "lock xadd" not in _FLAGS_UNDEFINED
    assert "xadd" in _RESULT_SNAPSHOT_SETTERS


def test_lock_xadd_publishes_the_sum():
    out = _lift("lock xadd", [REFCOUNT, EDX])
    assert "RECOMP_ATOMIC_ADD32" in out, out
    assert "_fa = _old + _add" in out, out
    # The source is overwritten with the old value only after the sum is taken.
    assert out.index("_fa = _old + _add") < out.index("edx = _old"), out


def test_jne_after_xadd_reads_the_snapshot():
    cond, _ = _make_condition("jne", "xadd", [REFCOUNT, EDX])
    assert cond == "(_fa != 0)", cond
    cond, _ = _make_condition("je", "xadd", [REFCOUNT, EDX])
    assert cond == "(_fa == 0)", cond
    cond, _ = _make_condition("js", "xadd", [REFCOUNT, EDX])
    assert cond == "(_fas < 0)", cond
    cond, _ = _make_condition("jg", "xadd", [REFCOUNT, EDX])
    assert cond == "(_fas > 0)", cond


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"  ok   {name}")
        except AssertionError as exc:
            failures += 1
            print(f"  FAIL {name}\n{exc}")
    print("xadd flags: " + ("OK" if not failures else f"{failures} FAILED"))
    sys.exit(1 if failures else 0)
