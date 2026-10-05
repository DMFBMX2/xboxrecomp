"""Differential test of one recompiled function against the original code.

    python tools/recomp/difftest.py snap_16DDF0_60_in.bin [--trace-calls]

Input is a pair of snapshots written by recomp_snap() (see a game project's
recomp_manual.c): guest registers and RAM at a function's entry, and again at
the instruction after the call returned. The entry snapshot is loaded into
Unicorn, the original x86 code is run from the function's first instruction
until it returns, and every byte it wrote is compared with the exit snapshot.

A difference means the translation of that function, or of something it
calls, does not do what the machine code does. Snapshot a callee to narrow it
down; --trace-calls lists the callees the original code went through.

Needs `pip install unicorn`. The callee's own dead stack frame is ignored,
and only pages the original code wrote are compared: other guest threads
keep running between the two snapshots and change unrelated memory.
--all-pages also compares pages only the recompiled side changed.
"""
import struct
import sys

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_WRITE,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_CODE, UC_PROT_ALL, UcError)
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP,
                               UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EIP)

PAGE = 0x1000
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
NAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


def load(path):
    data = open(path, "rb").read()
    regs = struct.unpack_from("<8I", data, 0)
    pages = {}
    off = 32
    st = None
    if data[32:36] == b"FX87":
        st = struct.unpack_from("<8d", data, 36)
        off = 100
    while off < len(data):
        addr = struct.unpack_from("<I", data, off)[0]
        pages[addr] = data[off + 4:off + 4 + PAGE]
        off += 4 + PAGE
    return regs, pages, st


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    trace_calls = "--trace-calls" in sys.argv
    path_in = args[0]
    path_out = path_in.replace("_in.bin", "_out.bin")
    fn = int(path_in.replace("\\", "/").split("/")[-1].split("_")[1], 16)

    regs_in, mem_in, st_in = load(path_in)
    regs_out, mem_out, st_out = load(path_out)

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    mapped = set()

    # Pages are mapped when the code first touches them, from the entry
    # snapshot or as zeros. Mapping all of an in-game snapshot up front takes
    # minutes; a function usually touches a few dozen pages.
    def ensure(addr):
        base = addr & ~(PAGE - 1)
        if base not in mapped:
            uc.mem_map(base, PAGE, UC_PROT_ALL)
            page = mem_in.get(base)
            if page:
                uc.mem_write(base, page)
            mapped.add(base)

    for reg, value in zip(REGS, regs_in):
        uc.reg_write(reg, value)

    esp_in = regs_in[4]
    ensure(esp_in)
    ensure(esp_in + 4)
    ensure(fn)
    ret = struct.unpack("<I", uc.mem_read(esp_in, 4))[0]
    written = set()

    def on_write(uc, access, addr, size, value, user):
        written.add(addr & ~(PAGE - 1))
        written.add((addr + size - 1) & ~(PAGE - 1))

    def on_unmapped(uc, access, addr, size, value, user):
        ensure(addr)
        ensure(addr + size - 1)
        return True

    calls = {}

    def on_code(uc, addr, size, user):
        if size == 5 and uc.mem_read(addr, 1) == b"\xe8":
            rel = struct.unpack("<i", uc.mem_read(addr + 1, 4))[0]
            target = (addr + 5 + rel) & 0xFFFFFFFF
            calls[target] = calls.get(target, 0) + 1

    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    if trace_calls:
        uc.hook_add(UC_HOOK_CODE, on_code)

    try:
        uc.emu_start(fn, ret)
    except UcError as e:
        print("emulation stopped: %s at eip=%08X" %
              (e, uc.reg_read(UC_X86_REG_EIP)))

    print("function %08X, returns to %08X" % (fn, ret))
    for name, reg, want in zip(NAMES, REGS, regs_out):
        got = uc.reg_read(reg)
        if name == "esp":
            want += 0   # exit snapshot is taken before the caller's cleanup
        flag = "" if got == want else "   <-- differs"
        if name in ("eax", "ebx", "esi", "edi", "esp", "ebp") or flag:
            print("  %s original=%08X recompiled=%08X%s" %
                  (name, got, want, flag if name != "ebp" else ""))
    # st(0): a float return value lives here. The entry snapshot's x87 stack
    # is not loaded into the emulator (the generated code keeps no depth, so
    # there is no telling how much of it is live), which is right for the
    # usual function that takes its arguments on the stack.
    if st_out is not None:
        from unicorn.x86_const import UC_X86_REG_ST0
        try:
            mant, exp = uc.reg_read(UC_X86_REG_ST0)
            e = exp & 0x7FFF
            val = mant * 2.0 ** (e - 16383 - 63) if e != 0x7FFF else float("nan")
            if exp & 0x8000:
                val = -val
        except Exception:
            val = float("nan")
        same = (val == st_out[0]) or abs(val - st_out[0]) <= 1e-6 * max(
            1.0, abs(val))
        print("  st0 original=%.9g recompiled=%.9g%s   (only meaningful if the "
              "function returns a float)" %
              (val, st_out[0], "" if same else "   <-- differs"))
    if trace_calls:
        for target, n in sorted(calls.items()):
            print("  calls %08X x %d" % (target, n))

    recomp_written = {a for a in set(mem_in) | set(mem_out)
                      if mem_in.get(a) != mem_out.get(a)}
    zero = bytes(PAGE)
    dead_lo, dead_hi = esp_in - 0x40000, esp_in + 4
    total = 0
    runs = []
    everything = "--all-pages" in sys.argv
    for base in sorted(written | recomp_written if everything else written):
        if base not in mapped:
            ensure(base)
        a = bytes(uc.mem_read(base, PAGE))
        b = mem_out.get(base, zero)
        if a == b:
            continue
        i = 0
        while i < PAGE:
            addr = base + i
            if a[i] != b[i] and not (dead_lo <= addr < dead_hi):
                j = i
                while j < PAGE and a[j] != b[j]:
                    j += 1
                runs.append((addr, a[i:j], b[i:j],
                             base in written, base in recomp_written))
                total += j - i
                i = j
            else:
                i += 1
    print("pages written: original %d, recompiled %d; differing bytes: %d in "
          "%d runs" % (len(written), len(recomp_written), total, len(runs)))
    for addr, a, b, w, rw in runs[:40]:
        print("  %08X +%-4d original=%s recompiled=%s%s" %
              (addr, len(a), a[:12].hex(), b[:12].hex(),
               "" if w else "  (original never wrote this page)"))
    if len(runs) > 40:
        print("  ... %d more" % (len(runs) - 40))


if __name__ == "__main__":
    main()
