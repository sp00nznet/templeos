"""Read the RedSea file system on a TempleOS ISO (host-side reader)."""
import sys, struct, os
BLK = 512
def boot(d):
    for o in range(0, len(d), BLK):
        if d[o+3] == 0x88 and d[o+510:o+512] == b'\x55\xaa':
            drv, sects, root, bm = struct.unpack_from('<qqqq', d, o+8)
            return drv, root
    raise SystemExit('no RedSea boot block')
def entries(d, clus):
    o = clus*BLK
    _, _, _, size = struct.unpack_from('<H38sqq', d, o)
    for p in range(o, o+size, 64):
        attr, name, c, sz = struct.unpack_from('<H38sqq', d, p)
        name = name.split(b'\0')[0].decode('latin1')
        if not name: continue
        yield attr, name, c, sz
def walk(d, clus, path=''):
    for attr, name, c, sz in entries(d, clus):
        if name in ('.', '..') or c == clus: continue
        if attr & 0x10: yield from walk(d, c, path+name+'/')
        else: yield path+name, c, sz, attr
if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    drv, root = boot(d)
    out = sys.argv[2] if len(sys.argv) > 2 else None
    for p, c, sz, attr in walk(d, root):
        if out:
            f = os.path.join(out, p); os.makedirs(os.path.dirname(f), exist_ok=True)
            open(f, 'wb').write(d[c*BLK:c*BLK+sz])
        else: print('%6x %8d %04x %s' % (c, sz, attr, p))
