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
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_RIP

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
STOP = {'ret', 'jmp', 'iretq', 'iret', 'iretd', 'ud2', 'hlt'}


def switch_targets(img, base, hist, jmp):
    """HolyC's switch: MOVSX/MOVZX/MOV r,[idx+TABLE]; ADD r32,BASE; JMP r.
    The table holds 1/2/4-byte offsets from BASE (the end of the JMP); the
    bound comes from the nearest CMP idx,N before it."""
    if len(hist) < 2:
        return [], True
    ld, add = hist[-2], hist[-1]
    if add.mnemonic != 'add' or ld.mnemonic not in ('movsx', 'movzx', 'mov', 'movsxd'):
        return [], True
    if len(add.operands) != 2 or add.operands[1].type != X86_OP_IMM:
        return [], True
    m = ld.operands[1]
    if m.type != X86_OP_MEM or not m.mem.index and not m.mem.base:
        return [], True
    width, table, signed = m.size, m.mem.disp, ld.mnemonic != 'movzx'
    bas = add.operands[1].imm & 0xFFFFFFFF
    idxreg = m.mem.index or m.mem.base
    n = None
    for k in range(len(hist) - 3, -1, -1):
        h = hist[k]
        if h.mnemonic == 'cmp' and len(h.operands) == 2 and h.operands[1].type == X86_OP_IMM \
                and h.operands[0].type == X86_OP_REG \
                and md.reg_name(h.operands[0].reg)[-2:] == md.reg_name(idxreg)[-2:]:
            jcc = hist[k + 1].mnemonic if k + 1 < len(hist) else ''
            # `jae default`/`jb table` leave 0..N-1; `ja default`/`jbe table` leave 0..N.
            n = h.operands[1].imm + (0 if jcc in ('jae', 'jnb', 'jnc', 'jb', 'jc', 'jnae', 'jge', 'jnl', 'jl', 'jnge') else 1)
            break
    bounded = n is not None
    if not bounded:
        n = 256       # `switch [x]` has no bounds check: read while the arms stay in the function
    if not (0 < n <= 4096):
        return [], True
    out = []
    for k in range(n):
        o = table - base + k * width
        if o < 0 or o + width > len(img):
            break
        t = (bas + int.from_bytes(img[o:o + width], 'little', signed=signed)) & 0xFFFFFFFF
        if not bounded and not (jmp.address - 0x10000 < t < table):
            continue      # not an arm of this function; each kept arm must still decode cleanly
        out.append(t)
    return out, bounded


def walk(img, base, entry, skip_targets, entries=frozenset()):
    """Reachable instructions of the function at `entry`: (end, calls, bad)."""
    seen, work, calls = set(), [(entry, 0)], set()
    opaque = False
    hist = []
    end = entry
    lo, hi = base, base + len(img)
    owner = {}                    # byte -> start of the instruction covering it
    while work:
        # Certain code first, then arms of bounded tables, then arms of
        # unbounded ones; arms in table order, where the real entries come
        # first. An arm that lands inside an instruction already decoded is
        # not an arm.
        cls = min(w[1] for w in work)
        # Arms (classes 1, 2) in table order; everything an arm reaches (class
        # 0.5, 1.5) depth first, before the table's next entry.
        k = (min(i for i, w in enumerate(work) if w[1] == cls) if cls in (1, 2)
             else max(i for i, w in enumerate(work) if w[1] == cls))
        a, spec = work.pop(k)
        hist = []
        while lo <= a < hi and a not in seen:
            try:
                ins = next(md.disasm(img[a - base:a - base + 16], a), None)
            except Exception:
                ins = None
            if ins is None or ins.mnemonic == '(bad)':
                if spec:          # an arm of an unbounded switch table that was not one
                    break
                return None
            if any(owner.get(b, a) != a for b in range(a, a + ins.size)):
                if spec:          # lands inside a real instruction: not an arm
                    break
            for b in range(a, a + ins.size):
                owner.setdefault(b, a)
            seen.add(a)
            end = max(end, a + ins.size)
            mn = ins.mnemonic
            if mn == 'jmp' and ins.operands and ins.operands[0].type == 1:   # jmp reg
                tg, bounded = switch_targets(img, base, hist, ins)
                if not tg:
                    opaque = True     # an indirect jump we cannot read: lift by linear sweep
                work.extend((t, 1 if bounded else 2) for t in tg if lo <= t < hi and (bounded or t > entry))
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
                if not (lo <= t < hi):
                    pass                  # outside the image: not code we can lift
                elif (t in entries and t != entry) or t < entry:
                    calls.add(t)          # a tail jump to another function
                elif lo <= t < hi:
                    work.append((t, spec - 0.5 if spec in (1, 2) else spec))
                if mn == 'jmp':
                    break
            if mn.split()[-1] in STOP and mn != 'jmp' or mn == 'jmp':
                break
            a += ins.size
    return end, calls, (None if opaque else seen)


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
        funcs[f] = (end - f, names.get(f, 'sub_%X' % f), seen)
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
