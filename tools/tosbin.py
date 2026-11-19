"""TempleOS .BIN modules (CBinFile): header, patch table, relocation.

relocate(raw, base) -> (image, exports): `image` is the file with its absolute
addresses patched for a load at `base` (the address of the CBinFile header),
as KStart32 (kernel) or Load() (modules) would do it; `exports` maps name ->
absolute address for REL exports and name -> value for IMM exports.
"""
import struct
HDR = 32
def header(raw):
    jmp, align, _, sig, org, pto, fsize = struct.unpack_from('<HBBIqqq', raw, 0)
    assert raw[4:8] == b'TOSB', raw[4:8]
    return dict(align=align, org=org, patch=pto, size=fsize)

def entries(raw):
    h = header(raw); p = h['patch']
    while True:
        et = raw[p]; p += 1
        if et == 0: return
        i = struct.unpack_from('<I', raw, p)[0]; p += 4
        e = raw.index(b'\0', p); name = raw[p:e].decode('latin1'); p = e + 1
        extra = []
        if et == 20:                       # IET_ABS_ADDR
            extra = list(struct.unpack_from('<%dI' % i, raw, p)); p += 4*i
        elif et in (21, 22):
            sz = struct.unpack_from('<i', raw, p)[0]; p += 4
            extra = [sz] + list(struct.unpack_from('<%dI' % i, raw, p)); p += 4*i
        elif et in (23, 24):
            sz = struct.unpack_from('<q', raw, p)[0]; p += 8
            extra = [sz] + list(struct.unpack_from('<%dI' % i, raw, p)); p += 4*i
        yield et, i, name, extra

def relocate(raw, base, imports=None):
    img = bytearray(raw)
    mb = base + HDR
    exports, unresolved, last = {}, [], ''
    for et, i, name, extra in entries(raw):
        if et == 20:
            for off in extra:
                v = struct.unpack_from('<I', img, HDR + off)[0]
                struct.pack_into('<I', img, HDR + off, (v + mb) & 0xFFFFFFFF)
        elif et in (16, 18): exports[name] = mb + i
        elif et in (17, 19): exports[name] = i
        elif 2 <= et <= 11:
            if name: last = name
            unresolved.append((et, i, last))
        elif et == 25: exports.setdefault('__MAIN', []).append(mb + i)
    # Imports of the module's own exports: Load() (LoadKernel for the kernel)
    # patches these at run time with exactly these values, so lift the result.
    still = []
    for et, i, name in unresolved:
        t = exports.get(name)
        if not isinstance(t, int):
            still.append((et, i, name)); continue
        site = mb + i
        if et == 8: struct.pack_into('<i', img, HDR + i, t - site - 4)
        elif et == 9: struct.pack_into('<I', img, HDR + i, t & 0xFFFFFFFF)
        elif et == 11: struct.pack_into('<q', img, HDR + i, t)
        elif et == 10: struct.pack_into('<q', img, HDR + i, t - site - 8)
        else: still.append((et, i, name))
    return bytes(img), exports, still

if __name__ == '__main__':
    import sys
    raw = open(sys.argv[1], 'rb').read()
    print(header(raw))
    from collections import Counter
    print(Counter(e[0] for e in entries(raw)))
    img, ex, un = relocate(raw, int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x7C00)
    print(len(ex), 'exports', len(un), 'imports')
    for n in ('KMain', 'MAlloc', 'SYS_KERNEL', 'CORE0_32BIT_INIT', 'SYS_ENTER_LONG_MODE', 'SYS_KERNEL_END'):
        print(n, hex(ex[n]) if isinstance(ex.get(n), int) else ex.get(n))
