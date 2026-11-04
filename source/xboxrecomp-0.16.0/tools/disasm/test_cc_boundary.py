"""Self-check for the CC-padding function boundary.

Run: py -3 -m pytest tools/disasm/test_cc_boundary.py

MSVC pads to the next function with int3 after a `ret` -- and equally after a
tail call, because "return f(x)" compiles to a bare `jmp f`. The boundary pass
only recognised `ret`, and only looked back three bytes, which is not far
enough to have seen a `jmp rel32` even if it had accepted one.

Half-Life 2's filesystem module has this at 0x0041FB00:

    0x0041FB00  jmp 0x0041F370      <- tail call, 5 bytes
    0x0041FB05  int3 x11            <- padding to the 16-byte boundary
    0x0041FB10  mov eax, [esp+8]    <- a real function, in a pointer table

Missing the boundary let sub_0041FAD0 run on to 0x0041FE40 and swallow it, so
the indirect call to 0x0041FB10 had no function to resolve to and was skipped
at runtime instead of made.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm import config  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402


class _Insn:
    def __init__(self, addr, size, mnemonic="mov", is_ret=False, is_jump=False):
        self.address = addr
        self.size = size
        self.end_address = addr + size
        self.mnemonic = mnemonic
        self.is_ret = is_ret
        self.is_jump = is_jump


class _Section:
    virtual_addr = 0x00410000


class _Image:
    def __init__(self, data):
        self.data = data

    def get_section_data(self, section):
        return self.data


class _Engine:
    def __init__(self, insns):
        self.instructions = {i.address: i for i in insns}

    def get_instruction(self, addr):
        return self.instructions.get(addr)

    def probes_as_prologue(self, addr):
        return True


def _run(terminator_mnemonic, terminator_size, is_ret, is_jump):
    """Lay out <filler><terminator><int3 padding><next function> and detect."""
    base = _Section.virtual_addr
    term_at = 0x30
    pad_at = term_at + terminator_size
    next_at = 0x40

    data = bytearray(b"\x90" * 0x60)
    for i in range(pad_at, next_at):
        data[i] = config.CC_PADDING

    insns = [
        _Insn(base + term_at, terminator_size, terminator_mnemonic,
              is_ret=is_ret, is_jump=is_jump),
        _Insn(base + next_at, 4),
    ]

    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    det.image = _Image(bytes(data))
    det._candidates = {}
    found = []
    det._add_candidate = lambda addr, conf, why: found.append(addr)
    det._pass_cc_boundaries(_Section())
    return found, base + next_at


def test_ret_before_padding_starts_a_function():
    found, expected = _run("ret", 1, is_ret=True, is_jump=False)
    assert expected in found, f"{expected:#x} not in {[hex(a) for a in found]}"


def test_tail_call_before_padding_starts_a_function():
    # jmp rel32 is 5 bytes -- the case the old three-byte back-scan could not
    # reach and would have rejected anyway.
    found, expected = _run("jmp", 5, is_ret=False, is_jump=True)
    assert expected in found, f"{expected:#x} not in {[hex(a) for a in found]}"


def test_short_tail_call_before_padding_starts_a_function():
    found, expected = _run("jmp", 2, is_ret=False, is_jump=True)
    assert expected in found, f"{expected:#x} not in {[hex(a) for a in found]}"


def test_padding_after_ordinary_code_is_not_a_boundary():
    # Padding that does not follow a terminator says nothing about where the
    # next function starts; accepting it would split functions at random.
    found, expected = _run("mov", 4, is_ret=False, is_jump=False)
    assert expected not in found, "split a function at non-terminating padding"


def test_callback_probe_stops_at_next_known_function():
    base = _Section.virtual_addr
    next_at = 0x40
    known_at = 0x50
    data = bytearray(b"\x90" * 0x60)
    data[0x31:next_at] = b"\xcc" * (next_at - 0x31)
    insns = [
        _Insn(base + 0x30, 1, "ret", is_ret=True),
        _Insn(base + next_at, 1),
    ]

    class ProbeEngine(_Engine):
        def __init__(self, instructions):
            super().__init__(instructions)
            self.probe_bounds = []

        def probes_as_prologue(self, addr):
            return False

        def probes_as_callback_body(self, addr, upper, **kwargs):
            self.probe_bounds.append((addr, upper))
            return False

    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = ProbeEngine(insns)
    det.image = _Image(bytes(data))
    det._candidates = {base + known_at: (1.0, "prologue")}
    det._pass_cc_boundaries(_Section())

    assert det.engine.probe_bounds == [(base + next_at, base + known_at)]


def test_callback_probe_stops_at_next_unvalidated_cc_boundary():
    base = _Section.virtual_addr
    data = bytearray(b"\x90" * 0x60)
    data[0x31:0x40] = b"\xcc" * 0x0F
    data[0x46:0x50] = b"\xcc" * 0x0A
    insns = [
        _Insn(base + 0x30, 1, "ret", is_ret=True),
        _Insn(base + 0x40, 1),
        _Insn(base + 0x45, 1, "ret", is_ret=True),
        _Insn(base + 0x50, 1),
    ]

    class ProbeEngine(_Engine):
        def __init__(self, instructions):
            super().__init__(instructions)
            self.probe_bounds = []

        def probes_as_prologue(self, addr):
            return False

        def probes_as_callback_body(self, addr, upper, **kwargs):
            self.probe_bounds.append((addr, upper))
            return False

    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = ProbeEngine(insns)
    det.image = _Image(bytes(data))
    det._candidates = {}
    det._pass_cc_boundaries(_Section())

    assert det.engine.probe_bounds == [
        (base + 0x40, base + 0x50),
        (base + 0x50, base + 0x60),
    ]
