"""Find the 64-bit functions in a relocated TempleOS BIN image.

Seeds: the exports whose bytes begin with HolyC's prologue (PUSH RBP; MOV
RBP,RSP), explicit entries, and work/seeds-<module>.txt (addresses the runtime
reported as dispatched-to but not lifted). Recursive descent from them follows
call rel32 into new functions and jumps inside each one; a function's extent is
its entry up to the end of the furthest instruction reachable from it.

Writes funcs.txt for lift64_sys: "0xADDR 0xSIZE name".
"""
import sys, os, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
STOP = {'ret', 'jmp', 'iretq', 'iret', 'iretd', 'ud2', 'hlt'}


def switch_targets(img, base, hist, jmp):
    """HolyC's switch: MOVSX/MOVZX/MOV r,[idx+TABLE]; ADD r32,BASE; JMP r.
    The table holds 1/2/4-byte offsets from BASE (the end of the JMP); the
    bound comes from the nearest CMP idx,N before it."""
    if len(hist) < 2:
        return []
    ld, add = hist[-2], hist[-1]
    if add.mnemonic != 'add' or ld.mnemonic not in ('movsx', 'movzx', 'mov', 'movsxd'):
        return []
    if len(add.operands) != 2 or add.operands[1].type != X86_OP_IMM:
        return []
    m = ld.operands[1]
    if m.type != X86_OP_MEM or not m.mem.index and not m.mem.base:
        return []
    width, table, signed = m.size, m.mem.disp, ld.mnemonic != 'movzx'
    bas = add.operands[1].imm & 0xFFFFFFFF
    idxreg = m.mem.index or m.mem.base
    n = None
    for h in reversed(hist[:-2]):
        if h.mnemonic == 'cmp' and len(h.operands) == 2 and h.operands[1].type == X86_OP_IMM                 and h.operands[0].type == 2 and md.reg_name(h.operands[0].reg)[-2:] == md.reg_name(idxreg)[-2:]:
            n = h.operands[1].imm + 1
            break
    if n is None or not (0 < n <= 4096):
        return []
    out = []
    for k in range(n):
        o = table - base + k * width
        if o < 0 or o + width > len(img):
            break
        v = int.from_bytes(img[o:o + width], 'little', signed=signed)
        out.append((bas + v) & 0xFFFFFFFF)
    return out


def walk(img, base, entry, skip_targets, entries=frozenset()):
    """Reachable instructions of the function at `entry`: (end, calls, bad)."""
    seen, work, calls = set(), [entry], set()
    hist = []
    end = entry
    lo, hi = base, base + len(img)
    while work:
        a = work.pop()
        hist = []
        while lo <= a < hi and a not in seen:
            try:
                ins = next(md.disasm(img[a - base:a - base + 16], a), None)
            except Exception:
                ins = None
            if ins is None:
                return None
            seen.add(a)
            end = max(end, a + ins.size)
            mn = ins.mnemonic
            if mn == 'jmp' and ins.operands and ins.operands[0].type == 1:   # jmp reg
                work.extend(t for t in switch_targets(img, base, hist, ins) if lo <= t < hi)
            hist = (hist + [ins])[-12:]
            if mn in ('(bad)',):
                return None
            if not (mn.startswith('j') or mn == 'call'):
                for op in ins.operands:          # a function's address taken
                    v = None
                    if op.type == X86_OP_IMM:
                        v = op.imm & 0xFFFFFFFFFFFFFFFF
                    elif op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and mn == 'lea':
                        v = a + ins.size + op.mem.disp
                    if v is not None and v in entries and v != entry and lo <= v < hi:
                        calls.add(v)
            if mn == 'call' and ins.operands[0].type == X86_OP_IMM:
                t = ins.operands[0].imm
                if a not in skip_targets and lo <= t < hi:
                    calls.add(t)
            if (mn.startswith('j') or mn.startswith('loop')) and ins.operands \
                    and ins.operands[0].type == X86_OP_IMM:
                t = ins.operands[0].imm
                if (t in entries and t != entry) or t < entry:
                    calls.add(t)          # a tail jump to another function
                elif lo <= t < hi:
                    work.append(t)
                if mn == 'jmp':
                    break
            if mn.split()[-1] in STOP and mn != 'jmp' or mn == 'jmp':
                break
            a += ins.size
    return end, calls, seen


def discover(img, base, seeds, skip_targets, names):
    funcs, todo, bad = {}, list(seeds), []
    while todo:
        f = todo.pop()
        if f in funcs:
            continue
        r = walk(img, base, f, skip_targets, names.keys() | set(seeds))
        if r is None:
            bad.append(f)
            continue
        end, calls, seen = r
        funcs[f] = (end - f, names.get(f, 'sub_%X' % f))
        todo.extend(c for c in calls if c not in funcs)
    return funcs, bad


def main():
    sys.path.insert(0, os.path.dirname(__file__))
    import tosbin
    binf, base, out = sys.argv[1], int(sys.argv[2], 0), sys.argv[3]
    extra = [int(x, 0) for x in sys.argv[4:] if not os.path.exists(x)]
    seedfiles = [x for x in sys.argv[4:] if os.path.exists(x)]
    raw = open(binf, 'rb').read()
    img, exports, imports = tosbin.relocate(raw, base)
    mb = base + tosbin.HDR
    skip = set()
    for et, off, name in imports:          # call sites the loader patches later
        skip.add(mb + off - 1)
    names = {v: k for k, v in exports.items() if isinstance(v, int)}
    seeds = set(extra)
    for k, v in exports.items():
        if isinstance(v, int) and base <= v < base + len(img) and img[v - base:v - base + 4] == b'\x55\x48\x8b\xec':
            seeds.add(v)
    for sf in seedfiles:
        for line in open(sf):
            line = line.split('#')[0].strip()
            if line:
                seeds.add(int(line.split()[0], 0))
    funcs, bad = discover(img, base, seeds, skip, names)
    with open(out, 'w') as f:
        for a in sorted(funcs):
            f.write('0x%X 0x%X %s\n' % (a, funcs[a][0], funcs[a][1]))
    print('%d functions (%d seeds), %d undecodable: %s' % (len(funcs), len(seeds), len(bad),
          ' '.join('%X' % b for b in bad[:10])))


if __name__ == '__main__':
    main()
