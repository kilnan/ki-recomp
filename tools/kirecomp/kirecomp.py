#!/usr/bin/env python3
"""Static MIPS III (R4600, little-endian) -> C++ translator for Killer Instinct.

Input : a flat code image loaded at a known virtual address (for KI 1 L1.5D
        the game's rom-0 image at 0x88000000, see docs/TECHNICAL_NOTE.md) and
        a list of function entry points.
Output: one C++ source with one host function per guest function plus the
        address -> function table used by ki::lookup_function.

The generated code relies on runtime/include/ki/runtime.h for the guest
memory map, arithmetic helpers and faults.

Scope of this first version (deliberately small, see the technical note):
- integer MIPS III ISA, branches incl. "likely", delay slots, jal/jr/jalr,
  traps, syscall/break, sync, cache and COP0 forwarded to the runtime;
- FPU (COP1) instructions are rejected unless --allow-unimplemented;
- jr to a register other than $ra goes through ki::call_indirect;
- function end detection is a linear scan (see find_function_end), which can
  be overridden with ADDR:END.

Decoding uses rabbitizer (MIT, the decoder N64Recomp uses) only to identify
the instruction; operand fields are taken directly from the instruction word.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from dataclasses import dataclass, field

import rabbitizer

rabbitizer.config.pseudos_enablePseudos = False
rabbitizer.config.regNames_gprAbiNames = rabbitizer.Abi.O32
rabbitizer.config.misc_omit0XOnSmallImm = False


class TranslateError(Exception):
    pass


@dataclass
class Insn:
    vaddr: int
    word: int
    name: str  # rabbitizer unique id without the "cpu_" prefix
    text: str

    @property
    def rs(self): return (self.word >> 21) & 31
    @property
    def rt(self): return (self.word >> 16) & 31
    @property
    def rd(self): return (self.word >> 11) & 31
    @property
    def sa(self): return (self.word >> 6) & 31
    @property
    def imm(self): return self.word & 0xFFFF
    @property
    def simm(self): return self.imm - 0x10000 if self.imm & 0x8000 else self.imm
    # COP1 fields
    @property
    def ft(self): return (self.word >> 16) & 31
    @property
    def fs(self): return (self.word >> 11) & 31
    @property
    def fd(self): return (self.word >> 6) & 31

    @property
    def branch_target(self): return (self.vaddr + 4 + (self.simm << 2)) & 0xFFFFFFFF
    @property
    def jump_target(self): return ((self.vaddr + 4) & 0xF0000000) | ((self.word & 0x03FFFFFF) << 2)


class Image:
    def __init__(self, data: bytes, vram: int):
        if len(data) % 4:
            raise TranslateError("image size is not a multiple of 4")
        self.data = data
        self.vram = vram
        self.sha1 = hashlib.sha1(data).hexdigest()

        self.extra: list[tuple[int, bytes]] = []  # read-only data images (jump tables)

    def add_data(self, data: bytes, vram: int):
        self.extra.append((vram, data))

    def contains(self, vaddr: int) -> bool:
        return self.vram <= vaddr < self.vram + len(self.data)

    def read_word(self, vaddr: int) -> int | None:
        for base, blob in [(self.vram, self.data), *self.extra]:
            if base <= vaddr and vaddr + 4 <= base + len(blob):
                return struct.unpack_from("<I", blob, vaddr - base)[0]
        return None

    def insn(self, vaddr: int) -> Insn:
        if not self.contains(vaddr):
            raise TranslateError(f"address {vaddr:08x} outside image")
        (word,) = struct.unpack_from("<I", self.data, vaddr - self.vram)  # little-endian guest
        ri = rabbitizer.Instruction(word, vaddr)
        name = ri.uniqueId.name
        if name.startswith("cpu_"):
            name = name[4:]
        return Insn(vaddr, word, name, ri.disassemble())


CONDITIONAL_BRANCHES = {
    "beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal",
    "beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bltzall", "bgezall",
    "bc1f", "bc1t", "bc1fl", "bc1tl",
}
LIKELY = {"beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bltzall", "bgezall", "bc1fl", "bc1tl"}
JUMPS = {"j", "jal", "jr", "jalr"}


def has_delay_slot(i: Insn) -> bool:
    return i.name in CONDITIONAL_BRANCHES or i.name in JUMPS


def _resolve_const(img: Image, reg: int, pc: int, lo: int) -> int | None:
    """Value of `reg` just before `pc`, if it is built by lui/addiu/ori in the
    straight-line code between `lo` and `pc` (no branch targets are checked:
    callers only use this right before a jr)."""
    value, pending = None, []
    a = pc - 4
    while a >= lo:
        i = img.insn(a)
        writes = i.rt if i.name in ("lui", "addiu", "ori", "lw", "lbu", "lhu", "andi") else (
            i.rd if i.name in ("addu", "sll", "or", "subu") else None)
        if i.name in CONDITIONAL_BRANCHES or i.name in JUMPS:
            if a < pc - 8:
                return None
        if writes == reg and reg != 0:
            if i.name == "lui":
                value = (i.imm << 16) & 0xFFFFFFFF
                for op, imm in reversed(pending):
                    value = (value + imm) & 0xFFFFFFFF if op == "add" else value | imm
                return value
            if i.name == "addiu" and i.rs == reg:
                pending.append(("add", i.simm))
            elif i.name == "ori" and i.rs == reg:
                pending.append(("or", i.imm))
            else:
                return None
        a -= 4
    return None


def jump_table(img: Image, jr: Insn, lo: int, hi: int, window: int = 48) -> tuple[int, list[int]] | None:
    """Recognise `lw rX, off(rA); jr rX` where rA = table base + index, the base
    being a lui/addiu constant. Returns (table address, targets): the table is
    read while its entries point into [lo, hi) (its length is not encoded)."""
    start = max(lo, jr.vaddr - window)
    reg, a = jr.rs, jr.vaddr - 4
    lw = None
    while a >= start:
        i = img.insn(a)
        if i.name == "lw" and i.rt == reg:
            lw = i
            break
        if reg in (i.rd if i.name in ("addu", "or", "sll") else -1, i.rt if i.name in ("lui", "addiu") else -1):
            return None
        a -= 4
    if lw is None:
        return None
    base_reg, a = lw.rs, lw.vaddr - 4
    while a >= start:
        i = img.insn(a)
        if i.name == "addu" and i.rd == base_reg:
            for cand in (i.rs, i.rt):
                c = _resolve_const(img, cand, i.vaddr, start)
                if c is not None:
                    table = (c + lw.simm) & 0xFFFFFFFF
                    targets = []
                    for k in range(1024):
                        w = img.read_word(table + 4 * k)
                        if w is None or not (lo <= w < hi) or w & 3:
                            break
                        targets.append(w)
                    return (table, targets) if targets else None
            return None
        if base_reg in (i.rd if i.name in ("or", "sll", "subu") else -1,
                        i.rt if i.name in ("lui", "addiu", "lw", "lbu") else -1):
            return None
        a -= 4
    return None


def find_function_end(img: Image, start: int, limit: int = 0x40000, stops: frozenset = frozenset()) -> int:
    """Linear scan: the function ends after the delay slot of the first
    `jr $ra` (or out-of-function `j`) that is not skipped over by an earlier
    forward branch inside the function. Functions that never return (e.g. a
    main loop) end where another known function starts (`stops`), provided no
    internal branch reaches past it. Returns the exclusive end address."""
    furthest = start
    pc = start
    image_end = img.vram + len(img.data)
    while pc < start + limit:
        if pc >= image_end:
            if pc >= furthest:
                return image_end
            raise TranslateError(f"function {start:08x} branches past the end of the image")
        if pc != start and pc in stops and pc > furthest:
            return pc
        i = img.insn(pc)
        if i.name in CONDITIONAL_BRANCHES:
            furthest = max(furthest, i.branch_target)
        elif i.name == "j":
            t = i.jump_target
            if start <= t <= pc + 0x1000:
                furthest = max(furthest, t)
            elif pc >= furthest:
                return pc + 8  # tail call
        elif i.name == "jr":
            if i.rs != 31:
                # Case targets lie before the next known function entry.
                nxt = min((x for x in stops if x > pc), default=start + limit)
                jt = jump_table(img, i, start, nxt)
                if jt:
                    furthest = max(furthest, max(jt[1]))
            if pc >= furthest:
                return pc + 8
        pc += 4
    raise TranslateError(f"no end found for function {start:08x}")


R = lambda n: f"c.r[{n}]"  # noqa: E731


def u32(v: int) -> str:
    return f"0x{v & 0xFFFFFFFF:08x}u"


def u64imm(v: int) -> str:
    return f"0x{v & 0xFFFFFFFFFFFFFFFF:016x}ull"


@dataclass
class Function:
    start: int
    end: int
    insns: list[Insn] = field(default_factory=list)


class Translator:
    def __init__(self, img: Image, allow_unimplemented: bool):
        self.img = img
        self.allow_unimplemented = allow_unimplemented
        self.known: set[int] = set()

    # -- straight-line instructions ---------------------------------------
    def assign(self, reg: int, expr: str) -> str:
        return f"{R(reg)} = {expr};" if reg != 0 else f"(void)({expr});"

    def simple(self, i: Insn) -> str:  # noqa: C901 - a dispatch table is clearer inline
        n, rs, rt, rd, sa, pc = i.name, R(i.rs), R(i.rt), i.rd, i.sa, u32(i.vaddr)
        simm, imm = i.simm, i.imm
        addr = f"ki::ea({rs}, {simm})"
        a = self.assign
        # shifts
        if n == "nop": return ";"
        if n == "sll": return a(rd, f"ki::sx32(ki::lo32({rt}) << {sa})")
        if n == "srl": return a(rd, f"ki::sx32(ki::lo32({rt}) >> {sa})")
        if n == "sra": return a(rd, f"ki::sra32({rt}, {sa})")
        if n == "sllv": return a(rd, f"ki::sx32(ki::lo32({rt}) << ({rs} & 31))")
        if n == "srlv": return a(rd, f"ki::sx32(ki::lo32({rt}) >> ({rs} & 31))")
        if n == "srav": return a(rd, f"ki::sra32({rt}, unsigned({rs} & 31))")
        if n == "dsll": return a(rd, f"{rt} << {sa}")
        if n == "dsrl": return a(rd, f"{rt} >> {sa}")
        if n == "dsra": return a(rd, f"ki::dsra({rt}, {sa})")
        if n == "dsll32": return a(rd, f"{rt} << {sa + 32}")
        if n == "dsrl32": return a(rd, f"{rt} >> {sa + 32}")
        if n == "dsra32": return a(rd, f"ki::dsra({rt}, {sa + 32})")
        if n == "dsllv": return a(rd, f"{rt} << ({rs} & 63)")
        if n == "dsrlv": return a(rd, f"{rt} >> ({rs} & 63)")
        if n == "dsrav": return a(rd, f"ki::dsra({rt}, unsigned({rs} & 63))")
        # add/sub
        if n == "addu": return a(rd, f"ki::sx32(ki::lo32({rs}) + ki::lo32({rt}))")
        if n == "subu": return a(rd, f"ki::sx32(ki::lo32({rs}) - ki::lo32({rt}))")
        if n == "daddu": return a(rd, f"{rs} + {rt}")
        if n == "dsubu": return a(rd, f"{rs} - {rt}")
        if n in ("add", "sub"):
            chk = "add32_overflows" if n == "add" else "sub32_overflows"
            op = "+" if n == "add" else "-"
            return (f"if (ki::{chk}(ki::lo32({rs}), ki::lo32({rt}))) ki::raise_fault(c, ki::Fault::Overflow, {pc}); "
                    + a(rd, f"ki::sx32(ki::lo32({rs}) {op} ki::lo32({rt}))"))
        if n in ("dadd", "dsub"):
            chk = "add64_overflows" if n == "dadd" else "sub64_overflows"
            op = "+" if n == "dadd" else "-"
            return (f"if (ki::{chk}({rs}, {rt})) ki::raise_fault(c, ki::Fault::Overflow, {pc}); "
                    + a(rd, f"{rs} {op} {rt}"))
        if n == "addiu": return a(i.rt, f"ki::sx32(ki::lo32({rs}) + {u32(simm)})")
        if n == "daddiu": return a(i.rt, f"{rs} + {u64imm(simm)}")
        if n == "addi":
            return (f"if (ki::add32_overflows(ki::lo32({rs}), {u32(simm)})) ki::raise_fault(c, ki::Fault::Overflow, {pc}); "
                    + a(i.rt, f"ki::sx32(ki::lo32({rs}) + {u32(simm)})"))
        if n == "daddi":
            return (f"if (ki::add64_overflows({rs}, {u64imm(simm)})) ki::raise_fault(c, ki::Fault::Overflow, {pc}); "
                    + a(i.rt, f"{rs} + {u64imm(simm)}"))
        # logic / compare
        if n == "and": return a(rd, f"{rs} & {rt}")
        if n == "or": return a(rd, f"{rs} | {rt}")
        if n == "xor": return a(rd, f"{rs} ^ {rt}")
        if n == "nor": return a(rd, f"~({rs} | {rt})")
        if n == "andi": return a(i.rt, f"{rs} & 0x{imm:x}ull")
        if n == "ori": return a(i.rt, f"{rs} | 0x{imm:x}ull")
        if n == "xori": return a(i.rt, f"{rs} ^ 0x{imm:x}ull")
        if n == "lui": return a(i.rt, f"ki::sx32({u32(imm << 16)})")
        if n == "slt": return a(rd, f"(ki::s64({rs}) < ki::s64({rt})) ? 1ull : 0ull")
        if n == "sltu": return a(rd, f"({rs} < {rt}) ? 1ull : 0ull")
        if n == "slti": return a(i.rt, f"(ki::s64({rs}) < {simm}ll) ? 1ull : 0ull")
        if n == "sltiu": return a(i.rt, f"({rs} < {u64imm(simm)}) ? 1ull : 0ull")
        # multiply / divide
        if n in ("mult", "multu", "dmult", "dmultu", "ddiv", "ddivu"):
            return f"ki::{n}(c, {rs}, {rt});"
        if n == "div": return f"ki::div32(c, {rs}, {rt});"
        if n == "divu": return f"ki::divu32(c, {rs}, {rt});"
        if n == "mfhi": return a(rd, "c.hi")
        if n == "mflo": return a(rd, "c.lo")
        if n == "mthi": return f"c.hi = {rs};"
        if n == "mtlo": return f"c.lo = {rs};"
        # loads (performed even when rt == $zero: I/O reads can have side effects)
        loads = {"lb": "ki::sx8(ki::load<uint8_t>(c, {a}, {pc}))",
                 "lbu": "uint64_t(ki::load<uint8_t>(c, {a}, {pc}))",
                 "lh": "ki::sx16(ki::load<uint16_t>(c, {a}, {pc}))",
                 "lhu": "uint64_t(ki::load<uint16_t>(c, {a}, {pc}))",
                 "lw": "ki::sx32(ki::load<uint32_t>(c, {a}, {pc}))",
                 "ll": "ki::sx32(ki::load<uint32_t>(c, {a}, {pc}))",
                 "lwu": "uint64_t(ki::load<uint32_t>(c, {a}, {pc}))",
                 "ld": "ki::load<uint64_t>(c, {a}, {pc})",
                 "lld": "ki::load<uint64_t>(c, {a}, {pc})",
                 "lwl": "ki::lwl(c, {rt}, {a}, {pc})", "lwr": "ki::lwr(c, {rt}, {a}, {pc})",
                 "ldl": "ki::ldl(c, {rt}, {a}, {pc})", "ldr": "ki::ldr(c, {rt}, {a}, {pc})"}
        if n in loads:
            return a(i.rt, loads[n].format(a=addr, pc=pc, rt=rt))
        stores = {"sb": "uint8_t", "sh": "uint16_t", "sw": "uint32_t", "sd": "uint64_t"}
        if n in stores:
            t = stores[n]
            return f"ki::store<{t}>(c, {addr}, static_cast<{t}>({rt}), {pc});"
        if n in ("swl", "swr", "sdl", "sdr"):
            return f"ki::{n}(c, {rt}, {addr}, {pc});"
        if n in ("sc", "scd"):  # single CPU, no LL/SC contention modelled
            t = "uint32_t" if n == "sc" else "uint64_t"
            return f"ki::store<{t}>(c, {addr}, static_cast<{t}>({rt}), {pc}); " + a(i.rt, "1ull")
        # traps
        traps = {"teq": "{rs} == {rt}", "tne": "{rs} != {rt}", "tge": "ki::s64({rs}) >= ki::s64({rt})",
                 "tgeu": "{rs} >= {rt}", "tlt": "ki::s64({rs}) < ki::s64({rt})", "tltu": "{rs} < {rt}",
                 "teqi": "{rs} == {i64}", "tnei": "{rs} != {i64}", "tgei": "ki::s64({rs}) >= {simm}ll",
                 "tgeiu": "{rs} >= {i64}", "tlti": "ki::s64({rs}) < {simm}ll", "tltiu": "{rs} < {i64}"}
        if n in traps:
            cond = traps[n].format(rs=rs, rt=rt, simm=simm, i64=u64imm(simm))
            return f"if ({cond}) ki::raise_fault(c, ki::Fault::Trap, {pc});"
        if n == "syscall": return f"ki::raise_fault(c, ki::Fault::Syscall, {pc});"
        if n == "break": return f"ki::raise_fault(c, ki::Fault::Break, {pc});"
        if n == "sync": return ";"
        if n == "cache": return f"ki::cache_op(c, {i.rt}, {addr}, {pc});"
        if n in ("mfc0", "dmfc0"):
            conv = "ki::sx32(ki::lo32({}))" if n == "mfc0" else "{}"
            return a(i.rt, conv.format(f"ki::mfc0(c, {i.rd}, {pc})"))
        if n in ("mtc0", "dmtc0"):
            return f"ki::mtc0(c, {i.rd}, {rt}, {pc});"
        if n in ("tlbr", "tlbwi", "tlbwr", "tlbp", "eret"):
            return f"ki::cop0_op(c, {u32(i.word)}, {pc});"
        fpu = self.fpu(i, addr, pc)
        if fpu is not None:
            return fpu
        if self.allow_unimplemented:
            return f"ki::raise_fault(c, ki::Fault::Unimplemented, {pc}); /* {n} */"
        raise TranslateError(f"{i.vaddr:08x}: unsupported instruction {n} ({i.text})")

    def fpu(self, i: Insn, addr: str, pc: str) -> str | None:
        """COP1, single precision and moves (Status.FR = 1). None if not FPU."""
        n, ft, fs, fd, rt = i.name, i.ft, i.fs, i.fd, R(i.rt)
        S = lambda r: f"ki::fs(c, {r})"  # noqa: E731
        if n == "lwc1": return f"ki::set_fbits(c, {ft}, ki::load<uint32_t>(c, {addr}, {pc}));"
        if n == "swc1": return f"ki::store<uint32_t>(c, {addr}, ki::fbits(c, {ft}), {pc});"
        if n == "ldc1": return f"c.f[{ft}] = ki::load<uint64_t>(c, {addr}, {pc});"
        if n == "sdc1": return f"ki::store<uint64_t>(c, {addr}, c.f[{ft}], {pc});"
        if n == "mtc1": return f"ki::set_fbits(c, {fs}, ki::lo32({rt}));"
        if n == "mfc1": return self.assign(i.rt, f"ki::sx32(ki::fbits(c, {fs}))")
        if n == "dmtc1": return f"c.f[{fs}] = {rt};"
        if n == "dmfc1": return self.assign(i.rt, f"c.f[{fs}]")
        if n == "ctc1": return f"ki::ctc1(c, {fs}, {rt});"
        if n == "cfc1": return self.assign(i.rt, f"ki::cfc1(c, {fs})")
        binop = {"add_s": "+", "sub_s": "-", "mul_s": "*", "div_s": "/"}
        if n in binop: return f"ki::set_fs(c, {fd}, {S(fs)} {binop[n]} {S(ft)});"
        if n == "sqrt_s": return f"ki::set_fs(c, {fd}, std::sqrt({S(fs)}));"
        if n == "abs_s": return f"ki::set_fbits(c, {fd}, ki::fbits(c, {fs}) & 0x7fffffffu);"
        if n == "neg_s": return f"ki::set_fbits(c, {fd}, ki::fbits(c, {fs}) ^ 0x80000000u);"
        if n == "mov_s": return f"ki::set_fbits(c, {fd}, ki::fbits(c, {fs}));"
        if n == "cvt_s_w": return f"ki::set_fs(c, {fd}, static_cast<float>(static_cast<int32_t>(ki::fbits(c, {fs}))));"
        if n == "cvt_w_s": return f"ki::set_fbits(c, {fd}, static_cast<uint32_t>(ki::cvt_w_s(c, {S(fs)})));"
        if n.startswith("c_") and n.endswith("_s"):
            return f"ki::set_fcc(c, ki::fcompare({i.word & 15}, {S(fs)}, {S(ft)}));"
        return None

    # -- control flow -------------------------------------------------------
    def branch_cond(self, i: Insn) -> str:
        rs, rt = R(i.rs), R(i.rt)
        if i.name in ("bc1f", "bc1fl"):
            return "!ki::fcc(c)"
        if i.name in ("bc1t", "bc1tl"):
            return "ki::fcc(c)"
        if i.name in ("beq", "beql", "bne", "bnel") and i.rs == i.rt:  # e.g. "b" = beq $zero,$zero
            return "true" if i.name.startswith("beq") else "false"
        base = {"beql": "beq", "bnel": "bne", "blezl": "blez", "bgtzl": "bgtz", "bltzl": "bltz",
                "bgezl": "bgez", "bltzal": "bltz", "bgezal": "bgez", "bltzall": "bltz",
                "bgezall": "bgez"}.get(i.name, i.name)
        return {"beq": f"{rs} == {rt}", "bne": f"{rs} != {rt}",
                "blez": f"ki::s64({rs}) <= 0", "bgtz": f"ki::s64({rs}) > 0",
                "bltz": f"ki::s64({rs}) < 0", "bgez": f"ki::s64({rs}) >= 0"}[base]

    def call(self, target: int, pc: int) -> str:
        if target in self.known:
            return f"func_{target:08x}(c);"
        return f"ki::call_indirect(c, {u32(target)}, {u32(pc)});"

    def translate(self, fn: Function) -> str:
        insns = fn.insns
        # Instructions may be non-contiguous when bounds come from control-flow
        # tracing (trace_function); "inside" means reached, not "in the range".
        addrs = {i.vaddr for i in insns}
        targets = set()
        tables: dict[int, tuple[int, list[int]]] = {}
        for i in insns:
            if i.name == "jr" and i.rs != 31:
                jt = jump_table(self.img, i, fn.start, fn.end)
                if jt:
                    tables[i.vaddr] = (jt[0], [t for t in jt[1] if t in addrs])
                    targets.update(tables[i.vaddr][1])
            if i.name in CONDITIONAL_BRANCHES and i.branch_target in addrs:
                targets.add(i.branch_target)
            if i.name == "j" and i.jump_target in addrs:
                targets.add(i.jump_target)
        # A computed jump we cannot resolve (hand-written code: jumps into
        # unrolled loops, continuations in registers) may land on any
        # instruction of the function, so every instruction gets a label and
        # the jump dispatches over all of them.
        unresolved = [i for i in insns if i.name == "jr" and i.rs != 31 and i.vaddr not in tables]
        if unresolved:
            targets.update(addrs)
        # A branch may target an instruction that is also the delay slot of a
        # preceding branch (loop rotation). Reached that way it is an ordinary
        # instruction, so it is emitted a second time, standalone, after the
        # branch sequence; the fall-through path jumps over that copy.
        slot_targets = set()
        for k, i in enumerate(insns[:-1]):
            if has_delay_slot(i) and insns[k + 1].vaddr in targets:
                slot_targets.add(insns[k + 1].vaddr)
                if insns[k + 1].vaddr + 4 in addrs:
                    targets.add(insns[k + 1].vaddr + 4)
        out = [f"void func_{fn.start:08x}(ki::Context& c) {{",
               f"    // guest 0x{fn.start:08x}-0x{fn.end:08x} ({len(insns)} instructions)",
               "    bool br = false;", "    (void)br;", "    (void)c;"]

        def emit(line: str, i: Insn | None = None):
            cmt = f"  // {i.vaddr:08x}: {i.word:08x}  {i.text}" if i else ""
            out.append(f"    {line}{cmt}")

        def fall_through(next_addr: int, next_idx: int, pc: int):
            # Control reaches next_addr sequentially; if that is not the next
            # emitted instruction, continue in the function that starts there
            # (shared tails, functions that do not return).
            if next_idx >= len(insns) or insns[next_idx].vaddr != next_addr:
                emit(f"{self.call(next_addr, pc)} return;  // fall through to 0x{next_addr:08x}")

        idx = 0
        while idx < len(insns):
            i = insns[idx]
            if i.vaddr in targets:
                out.append(f"L_{i.vaddr:08x}:")
            if not has_delay_slot(i):
                emit(self.simple(i), i)
                idx += 1
                fall_through(i.vaddr + 4, idx, i.vaddr)
                continue
            if idx + 1 >= len(insns) or insns[idx + 1].vaddr != i.vaddr + 4:
                raise TranslateError(f"{i.vaddr:08x}: delay slot outside function")
            d = insns[idx + 1]
            if has_delay_slot(d):
                # Branch in a delay slot (UNPREDICTABLE in the ISA). The game
                # uses it only as `bnez x,L; beqz x,L`-style pairs whose
                # conditions exclude each other (0x880322d4, 0x880106b8, and the
                # boot ROM decoder): if the first is taken, the second is not and
                # control goes to the first target; otherwise the second runs as
                # an ordinary branch with its own delay slot. Both taken traps.
                plain = CONDITIONAL_BRANCHES - LIKELY - {"bltzal", "bgezal"}
                if i.name not in plain or d.name not in plain:
                    raise TranslateError(f"{d.vaddr:08x}: control transfer in delay slot")
                t = i.branch_target
                go = f"goto L_{t:08x};" if t in addrs else f"{{ {self.call(t, i.vaddr)} return; }}"
                emit(f"br = {self.branch_cond(i)};", i)
                emit(f"if (br) {{ if ({self.branch_cond(d)}) ki::raise_fault(c, ki::Fault::Unimplemented, "
                     f"{u32(d.vaddr)}); {go} }}")
                idx += 1
                continue
            ret = u32(i.vaddr + 8)
            dslot = lambda: emit("    " + self.simple(d), d)  # noqa: E731
            if i.name in CONDITIONAL_BRANCHES:
                t = i.branch_target
                go = f"goto L_{t:08x};" if t in addrs else f"{{ {self.call(t, i.vaddr)} return; }}"
                link = i.name in ("bltzal", "bgezal", "bltzall", "bgezall")
                emit(f"br = {self.branch_cond(i)};", i)
                if link:
                    emit(f"c.r[31] = ki::sx32({ret});")
                if i.name in LIKELY:
                    emit("if (br) {")
                    dslot()
                    emit(f"    {go}")
                    emit("}")
                else:
                    dslot()
                    emit(f"if (br) {go}")
            elif i.name == "j":
                t = i.jump_target
                emit(f"// j 0x{t:08x}", i)
                dslot()
                if t in addrs:
                    emit(f"goto L_{t:08x};")
                else:
                    emit(f"{self.call(t, i.vaddr)} return;  // tail call")
            elif i.name == "jal":
                t = i.jump_target
                emit(f"c.r[31] = ki::sx32({ret});", i)
                dslot()
                emit(self.call(t, i.vaddr))
            elif i.name == "jr":
                if i.rs == 31:
                    # Assumes $ra still holds the caller's return address, as
                    # N64Recomp does; MIPS code generated by compilers obeys this.
                    emit("// jr $ra", i)
                    dslot()
                    emit("return;")
                elif i.vaddr in tables:
                    table, tts = tables[i.vaddr]
                    emit(f"{{ uint32_t tgt = ki::lo32({R(i.rs)});  // jump table at 0x{table:08x}", i)
                    dslot()
                    emit("  switch (tgt) {")
                    for t in sorted(set(tts)):
                        emit(f"    case {u32(t)}: goto L_{t:08x};")
                    emit(f"    default: ki::call_indirect(c, tgt, {u32(i.vaddr)}); return;")
                    emit("  } }")
                else:
                    emit(f"{{ uint32_t tgt = ki::lo32({R(i.rs)});  // computed jump: any instruction", i)
                    dslot()
                    emit("  switch (tgt) {")
                    for a in sorted(addrs):
                        emit(f"    case {u32(a)}: goto L_{a:08x};")
                    emit(f"    default: ki::call_indirect(c, tgt, {u32(i.vaddr)}); return;")
                    emit("  } }")
            elif i.name == "jalr":
                emit(f"{{ uint32_t tgt = ki::lo32({R(i.rs)});", i)
                if i.rd:
                    emit(f"  c.r[{i.rd}] = ki::sx32({ret});")
                dslot()
                emit(f"  ki::call_indirect(c, tgt, {u32(i.vaddr)}); }}")
            if i.name not in ("j", "jr"):
                fall_through(i.vaddr + 8, idx + 2, i.vaddr)
            if d.vaddr in slot_targets:
                # Fall-through past the branch sequence skips the standalone copy.
                emit(f"goto L_{d.vaddr:08x}_skip;")
                out.append(f"L_{d.vaddr:08x}:")
                emit(self.simple(d), d)
                fall_through(d.vaddr + 4, idx + 2, d.vaddr)
                out.append(f"L_{d.vaddr:08x}_skip: ;")
            idx += 2
        out.append("}")
        return "\n".join(out)


def trace_function(img: Image, start: int, stops: frozenset = frozenset(), limit: int = 30000) -> list[int]:
    """Bounds by reachability: follow branches, in-function jumps and jump
    tables from the entry; calls return, `jr $ra` ends a path, jumps to another
    known entry are tail calls. Avoids decoding data embedded in code, which
    the linear scan of find_function_end runs into."""
    seen, work = set(), [start]
    while work:
        a = work.pop()
        if a in seen:
            continue
        if not img.contains(a):
            raise TranslateError(f"function {start:08x} reaches {a:08x} outside the image")
        if a != start and a in stops:
            continue  # sequential flow into another function: emitted as a tail call
        seen.add(a)
        if len(seen) > limit:
            raise TranslateError(f"function {start:08x} exceeds {limit} instructions")
        i = img.insn(a)
        if i.name == "INVALID" or (i.name not in CONDITIONAL_BRANCHES and i.name not in JUMPS
                                   and not img.insn(a).text.strip()):
            raise TranslateError(f"{a:08x}: invalid instruction reached")
        if not has_delay_slot(i):
            work.append(a + 4)
            continue
        work.append(a + 4)  # delay slot
        seen.add(a + 4)
        if i.name in CONDITIONAL_BRANCHES:
            work += [i.branch_target, a + 8]
        elif i.name == "j":
            if i.jump_target == start or i.jump_target not in stops:
                work.append(i.jump_target)
        elif i.name in ("jal", "jalr"):
            work.append(a + 8)
        elif i.name == "jr" and i.rs != 31:
            jt = jump_table(img, i, start, start + 0x40000)
            if jt:
                work += jt[1]
    return sorted(seen)


def discover_jal_targets(img: Image) -> set[int]:
    targets = set()
    for off in range(0, len(img.data), 4):
        (word,) = struct.unpack_from("<I", img.data, off)
        if word >> 26 == 3:  # jal
            t = ((img.vram + off + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            if img.contains(t):
                targets.add(t)
    return targets


def discover_code_pointers(img: Image) -> set[int]:
    """Words in the data images that point at code in the image: candidate
    entry points for jalr / function pointers (e.g. the blend routines inside
    the drawing code, called through the table at 0x880356a4). Jump-table
    cases also look like this; callers exclude the ones they recognise."""
    out = set()
    for base, blob in img.extra:
        for off in range(0, len(blob) - 3, 4):
            (w,) = struct.unpack_from("<I", blob, off)
            if img.contains(w) and not w & 3:
                out.add(w)
    return out


def parse_func_spec(s: str) -> tuple[int, int | None]:
    if ":" in s:
        a, b = s.split(":", 1)
        return int(a, 16), int(b, 16)
    return int(s, 16), None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--image", required=True, help="flat little-endian code image")
    ap.add_argument("--vram", required=True, type=lambda x: int(x, 16), help="load address (hex)")
    ap.add_argument("--func", action="append", default=[], metavar="ADDR[:END]",
                    help="function entry (hex), optional exclusive end; repeatable")
    ap.add_argument("--discover", action="store_true",
                    help="also translate every jal target found in the image (survey mode: "
                         "functions that fail to translate are reported and skipped)")
    ap.add_argument("--discover-pointers", action="store_true",
                    help="also translate code addresses found as words in the data images "
                         "(function pointers), excluding recognised jump-table cases")
    ap.add_argument("--data", action="append", default=[], metavar="FILE@VRAM",
                    help="read-only data image (for jump tables); repeatable")
    ap.add_argument("--expect-sha1", help="refuse to translate a different image")
    ap.add_argument("--allow-unimplemented", action="store_true")
    ap.add_argument("--out", required=True, help="output .cpp")
    ap.add_argument("--listing", help="optional text listing of translated functions")
    args = ap.parse_args(argv)

    img = Image(open(args.image, "rb").read(), args.vram)
    if args.expect_sha1 and img.sha1 != args.expect_sha1.lower():
        print(f"image sha1 {img.sha1} != expected {args.expect_sha1}", file=sys.stderr)
        return 2

    for spec in args.data:
        path, vram = spec.rsplit("@", 1)
        img.add_data(open(path, "rb").read(), int(vram, 16))
    tr = Translator(img, args.allow_unimplemented)
    specs = [parse_func_spec(s) for s in args.func]
    if args.discover:
        explicit = {a for a, _ in specs}
        specs += [(t, None) for t in sorted(discover_jal_targets(img)) if t not in explicit]
    if args.discover_pointers:
        explicit = {a for a, _ in specs}
        cases = set()
        for off in range(0, len(img.data), 4):
            i = img.insn(img.vram + off)
            if i.name == "jr" and i.rs != 31:
                jt = jump_table(img, i, img.vram, img.vram + len(img.data))
                if jt:
                    cases.update(jt[1])
        ptrs = sorted(discover_code_pointers(img) - cases - explicit)
        print(f"{len(ptrs)} code pointers in data images used as extra entries", file=sys.stderr)
        specs += [(t, None) for t in ptrs]
    stops = frozenset(discover_jal_targets(img) | {a for a, _ in specs})
    funcs, failures = [], []
    for start, end in specs:
        try:
            if end is not None:
                funcs.append(Function(start, end, [img.insn(a) for a in range(start, end, 4)]))
                continue
            try:
                end = find_function_end(img, start, stops=stops)
                fn = Function(start, end, [img.insn(a) for a in range(start, end, 4)])
                Translator(img, False).translate(fn)  # probe: falls back to tracing on failure
            except TranslateError:
                addrs = trace_function(img, start, stops)
                fn = Function(start, max(addrs) + 4, [img.insn(a) for a in addrs])
            funcs.append(fn)
        except TranslateError as e:
            if not (args.discover or args.discover_pointers):
                raise
            failures.append((start, f"boundary: {e}"))
    for fn in funcs:
        tr.known.add(fn.start)
    funcs.sort(key=lambda f: f.start)

    body, ok_funcs = [], []
    for fn in funcs:
        try:
            body.append(tr.translate(fn))
            ok_funcs.append(fn)
        except TranslateError as e:
            if not (args.discover or args.discover_pointers):
                raise
            failures.append((fn.start, str(e)))
    if failures:
        tr.known -= {a for a, _ in failures}
        body = [tr.translate(fn) for fn in ok_funcs]  # re-emit: calls to failed functions go indirect
    funcs = ok_funcs
    src = [
        "// Generated by tools/kirecomp/kirecomp.py -- do not edit.",
        f"// Source image sha1 {img.sha1}, loaded at 0x{img.vram:08x}.",
        "// Derived from the original game code: do not redistribute.",
        '#include "ki/runtime.h"', "",
        "namespace {",
        *[f"void func_{fn.start:08x}(ki::Context& c);" for fn in funcs], "",
        *body,
        "}  // namespace", "",
        "const ki::FuncEntry ki::kFunctionTable[] = {",
        *[f"    {{0x{fn.start:08x}u, func_{fn.start:08x}}}," for fn in funcs],
        "};",
        f"const std::size_t ki::kFunctionTableSize = {len(funcs)};", "",
    ]
    with open(args.out, "w") as f:
        f.write("\n".join(src))
    if args.listing:
        with open(args.listing, "w") as f:
            for fn in funcs:
                f.write(f"function 0x{fn.start:08x}-0x{fn.end:08x} ({len(fn.insns)} insns)\n")
                for i in fn.insns:
                    f.write(f"  {i.vaddr:08x}: {i.word:08x}  {i.text}\n")
    if args.discover or args.discover_pointers:
        n = sum(len(f.insns) for f in funcs)
        print(f"translated {len(funcs)} functions ({n} instructions), {len(failures)} failed", file=sys.stderr)
        reasons: dict[str, int] = {}
        for addr, why in failures:
            key = why.split(": ", 1)[-1].split(" (")[0]
            reasons[key] = reasons.get(key, 0) + 1
        for why, count in sorted(reasons.items(), key=lambda kv: -kv[1]):
            print(f"  {count:4d}  {why}", file=sys.stderr)
    else:
        for fn in funcs:
            print(f"translated 0x{fn.start:08x}-0x{fn.end:08x} ({len(fn.insns)} instructions)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except TranslateError as e:
        print(f"kirecomp: error: {e}", file=sys.stderr)
        sys.exit(1)
