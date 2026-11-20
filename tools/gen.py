"""Generate the lifted kernel from the user's TempleOS ISO.

  python tools/gen.py <TempleOS.ISO> [pcrecomp-dir]

Reads the kernel the DVD boot loader would load, relocates it for 0x7C00 as
KStart32 does, finds its 64-bit functions (tools/discover.py, plus the
addresses earlier runs reported in work/seeds-kernel.txt), lifts them with
pcrecomp's lift64_sys, and writes src/gen/:
  kernel_NN.c   the lifted functions, in chunks the compiler can take in parallel
  dispatch.c    guest address -> lifted function, and the hooks
  ksyms.h       the kernel's exports and boot constants, for runtime/main.c
Nothing from the ISO is copied into the repository; src/gen/ is ignored.
"""
import os, re, sys, struct
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import tosbin, discover
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

BASE = 0x7C00
CHUNK = 80
HOOKS = {'_YIELD': 'hook_yield', '_FREE': 'hook_free', 'COCCompile': 'hook_coccompile'}


def kernel_from_iso(iso):
    bl = iso[21 * 2048:22 * 2048]
    msg = b'64-bit capable processor.\n\r\0'
    i = bl.index(msg) + len(msg)
    lo, hi, cnt, shift = struct.unpack_from('<HHHH', bl, i)
    off = (lo | hi << 16) * 2048 + shift * 512
    size = struct.unpack_from('<q', iso, off + 24)[0]
    return iso[off:off + size]


def fnv(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def boot_constants(img, ex):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    out = {}
    a = ex['CORE0_32BIT_INIT']
    for i in md.disasm(img[a - BASE:a - BASE + 0x100], a):
        if i.mnemonic == 'call' and int(i.op_str, 16) == ex['SYS_ENTER_LONG_MODE']:
            out['KSTART64'] = i.address + i.size
            break
    a = ex['SYS_INIT_PAGE_TABLES']
    prev = None
    for i in md.disasm(img[a - BASE:a - BASE + 0x200], a):
        if prev and prev.op_str == 'edi, 0x100000' and i.mnemonic == 'xor':
            pass
        if i.mnemonic == 'mov' and i.op_str.startswith('ecx, 0x') and prev and prev.mnemonic == 'xor':
            out['SIZEOF_SYS_FIXED_AREA'] = int(i.op_str.split(',')[1], 16) * 4
            break
        prev = i
    a = ex['SYS_INIT_16MEG_SYS_CODE_BP']
    i = next(md.disasm(img[a - BASE:a - BASE + 16], a))
    out['SYS_FIXED_AREA_SYS_CODE_BP'] = int(i.op_str.split(',')[1], 16)
    return out


def main():
    iso = open(sys.argv[1], 'rb').read()
    pcr = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(ROOT), 'pcrecomp-temple')
    sys.path.insert(0, os.path.join(pcr, 'tools', 'lift'))
    import lift64_sys

    raw = kernel_from_iso(iso)
    img, ex, imports = tosbin.relocate(raw, BASE)
    consts = boot_constants(img, ex)
    seeds_all = []
    sf = os.path.join(ROOT, 'work', 'seeds.txt')
    if os.path.exists(sf):
        for line in open(sf):
            line = line.split('#')[0].strip()
            if line:
                seeds_all.append(int(line.split()[0], 0))

    # Regions: the kernel, then every module captured at run time (work/jit).
    regions = [dict(tag='kernel', base=BASE, img=img, ex=ex, imports=imports,
                    extra={consts['KSTART64']}, fnv=None)]
    idx = os.path.join(ROOT, 'work', 'jit', 'index.txt')
    if os.path.exists(idx):
        for line in open(idx):
            p = line.split()
            if len(p) == 3 and p[0] == 'mod':
                mbase = int(p[1], 16)
                mimg = open(os.path.join(ROOT, 'work', 'jit', 'mod_%X.bin' % mbase), 'rb').read()
                mex, mimp, mains = module_symbols(mimg, mbase)
                regions.append(dict(tag='mod_%X' % mbase, base=mbase, img=mimg, ex=mex, imports=mimp,
                                    extra=set(mains), fnv=fnv(mimg)))
            elif len(p) == 4 and p[0] == 'blob':
                bbase, bsize, bfnv = int(p[1], 16), int(p[2], 16), int(p[3], 16)
                bimg = open(os.path.join(ROOT, 'work', 'jit', 'blob_%X_%016X.bin' % (bbase, bfnv)), 'rb').read()
                if any(r.get('blob') == (bbase, bfnv) for r in regions):
                    continue
                regions.append(dict(tag='blob_%X_%08X' % (bbase, bfnv & 0xFFFFFFFF), base=bbase, img=bimg, ex={},
                                    imports=[], extra={bbase}, fnv=None, blob=(bbase, bfnv)))

    gen = os.path.join(ROOT, 'src', 'gen')
    os.makedirs(gen, exist_ok=True)
    allfuncs, allsyms, files = {}, [], []
    for r in regions:
        base, rimg, rex = r['base'], r['img'], r['ex']
        mb = base + tosbin.HDR
        dyn = {mb + off - 1 for et, off, name in r['imports'] if et == 8}
        names = {v: k for k, v in rex.items() if isinstance(v, int)}
        seeds = set(r['extra'])
        for k, v in rex.items():
            if isinstance(v, int) and base <= v < base + len(rimg) and rimg[v - base:v - base + 4] == b'\x55\x48\x8b\xec':
                seeds.add(v)
        seeds |= {a for a in seeds_all if base <= a < base + len(rimg)}
        funcs, bad = discover.discover(rimg, base, seeds, dyn, names)
        buckets = {}
        for a in funcs:
            buckets.setdefault((a - base) // 0x4000, {})[a] = funcs[a]
        for b, part in sorted(buckets.items()):
            fn = '%s_%02d.c' % (r['tag'], b)
            tmp = os.path.join(gen, fn + '.tmp')
            lift64_sys.lift_raw(rimg, base, part, tmp, header='temple_lift.h', dyn_calls=dyn,
                                dyn_all='blob' in r)
            text = open(tmp).read()
            if 'blob' in r:     # variants share addresses: give their functions their own names
                pre = 'B%08X_' % (r['blob'][1] & 0xFFFFFFFF)
                text = re.sub(r'^void L_([0-9A-F]{12})\(CPU \*c\)', 'void ' + pre + r'\1(CPU *c)', text, flags=re.M)
            write_if_changed(os.path.join(gen, fn), text)
            os.remove(tmp)
            files.append('src/gen/' + fn)
        if 'blob' in r:
            r['funcs'] = sorted(funcs)
        else:
            allfuncs.update(funcs)
        allsyms += [(v, k) for k, v in rex.items() if isinstance(v, int) and base <= v < base + len(rimg)]
        print('%s: %d functions at %#x%s' % (r['tag'], len(funcs), base,
              ', undecodable seeds ' + ' '.join('%X' % x for x in bad[:8]) if bad else ''))

    addrs = sorted(allfuncs)
    out = ['/* generated by tools/gen.py - do not edit */\n#include "temple_lift.h"\n#include <stddef.h>\n\n']
    out += ['void L_%012X(CPU *c);\n' % a for a in addrs]
    out += ['void %s(CPU *c);\n' % h for h in sorted(set(HOOKS.values()))]
    # Hooks: the dispatch table sends these to runtime wrappers, which call
    # the lifted body through g_body_<name>.
    allex = {}
    for r in regions:
        allex.update({k: v for k, v in r['ex'].items() if isinstance(v, int)})
    out.append('typedef void (*lifted_fn)(CPU *);\n')
    for name in sorted(HOOKS):
        out.append('lifted_fn g_body_%s = %s;\n' % (name, 'L_%012X' % allex[name] if allex.get(name) in allfuncs else '0'))
    hooked = {allex[k]: v for k, v in HOOKS.items() if allex.get(k) in allfuncs}
    blobs = [r for r in regions if 'blob' in r]
    for i, r in enumerate(blobs):
        pre = 'B%08X_' % (r['blob'][1] & 0xFFFFFFFF)
        out += ['void %s%012X(CPU *c);\n' % (pre, a) for a in r['funcs']]
        out.append('static const struct { uint64_t a; lifted_fn fn; } be_%d[] = {\n' % i)
        out += ['  {0x%Xull, %s%012X},\n' % (a, pre, a) for a in r['funcs']]
        out.append('};\n')
    out.append('typedef struct { uint64_t addr, size, fnv; const void *ents; size_t nents; } Blob;\nconst Blob g_blobs[] = {\n')
    out += ['  {0x%Xull, 0x%Xull, 0x%016Xull, be_%d, %d},\n' % (r['base'], len(r['img']), r['blob'][1], i, len(r['funcs']))
            for i, r in enumerate(blobs)]
    out.append('  {0, 0, 0, 0, 0}\n};\nconst size_t g_nblobs = %d;\n\n' % len(blobs))
    out.append('typedef struct { uint64_t a; void (*fn)(CPU *); } Ent;\nconst Ent g_lifted[] = {\n')
    out += ['  {0x%Xull, %s},\n' % (a, hooked.get(a, 'L_%012X' % a)) for a in addrs]
    out.append('};\nconst size_t g_nlifted = %d;\n\n' % len(addrs))
    syms = sorted(set(allsyms))
    out.append('typedef struct { uint64_t a; const char *n; } Sym;\nconst Sym g_syms[] = {\n')
    out += ['  {0x%Xull, "%s"},\n' % (v, k) for v, k in syms]
    out.append('};\nconst size_t g_nsyms = %d;\n\n' % len(syms))
    mods = [r for r in regions if r['fnv'] is not None]
    out.append('typedef struct { uint64_t base, size, fnv; } Mod;\nconst Mod g_mods[] = {\n')
    out += ['  {0x%Xull, 0x%Xull, 0x%016Xull},\n' % (r['base'], len(r['img']), r['fnv']) for r in mods]
    out.append('  {0, 0, 0}\n};\nconst size_t g_nmods = %d;\n' % len(mods))
    write_if_changed(os.path.join(gen, 'dispatch.c'), ''.join(out))
    k = ['/* generated by tools/gen.py - do not edit */\n#pragma once\n',
         '#define K_IMAGE_FNV 0x%016Xull\n' % fnv(raw), '#define K_IMAGE_END 0x%Xull\n' % (BASE + len(img))]
    k += ['#define K_%s 0x%Xull\n' % (n, v) for n, v in sorted(consts.items())]
    k += ['#define K_%s 0x%Xull\n' % (n, v) for n, v in sorted(ex.items()) if isinstance(v, int) and n.isidentifier()]
    write_if_changed(os.path.join(gen, 'ksyms.h'), ''.join(k))
    for f in os.listdir(gen):
        if f.endswith('.c') and f != 'dispatch.c' and 'src/gen/' + f not in files:
            os.remove(os.path.join(gen, f))
    write_if_changed(os.path.join(gen, 'kernel_files.cmake'), 'set(KERNEL_GEN %s)\n' % ' '.join(files))
    print('%d functions in %d files; KStart64 at %#x' % (len(addrs), len(files), consts['KSTART64']))


def module_symbols(mimg, mbase):
    """Exports, imports and IET_MAIN entries of a module image captured from memory."""
    mb = mbase + tosbin.HDR
    ex, imports, mains, last = {}, [], [], ''
    for et, i, name, extra in tosbin.entries(mimg):
        if et in (16, 18): ex[name] = mb + i
        elif et == 25: mains.append(mb + i)
        elif 2 <= et <= 11:
            if name: last = name
            imports.append((et, i, last))
    return ex, imports, mains


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    open(path, 'w').write(text)


if __name__ == '__main__':
    main()
