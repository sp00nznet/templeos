"""Expand TempleOS .Z files (CArcCompress, LZW), a port of Kernel/Compress.HC."""
import struct
MAXB = 12
def expand(arc):
    csize, esize, ctype = struct.unpack_from('<qqB', arc, 0)
    if ctype == 1: return arc[17:17+esize]
    assert ctype in (2, 3), ctype
    N = 1 << MAXB
    base = [0]*N; chs = [0]*N
    nxt = [None]*(2*N)            # nodes 0..N-1 entries, N+b = hash head for b
    minb = 7 if ctype == 2 else 8
    mte = 1 << minb
    st = dict(free_idx=mte, nbits=minb+1, cur=None, nextent=None, cbits=0, used=True)
    st['limit'] = 1 << st['nbits']
    def get():
        if not st['used']: return
        i = st['free_idx']; st['used'] = False
        st['cur'] = st['nextent']; st['cbits'] = st['nbits']
        if st['nbits'] < MAXB:
            st['nextent'] = i; i += 1
            if i == st['limit']:
                st['nbits'] += 1; st['limit'] = 1 << st['nbits']
        else:
            while True:
                i += 1
                if i == st['limit']: i = mte
                if nxt[N+i] is None: break
            st['nextent'] = i
            t = N + base[i]
            while t is not None:
                if nxt[t] == i: nxt[t] = nxt[i]; break
                t = nxt[t]
        st['free_idx'] = i
    get(); st['used'] = True
    data = int.from_bytes(arc[:csize] + b'\0\0\0\0', 'little')
    def field(pos, n): return (data >> pos) & ((1 << n) - 1)
    pos = 17*8; srcsz = csize*8
    out = bytearray()
    last = field(pos, st['nbits']); pos += st['nbits']
    out.append(last); get(); last_ch = last
    while len(out) < esize and pos + st['nbits'] <= srcsz:
        bc = field(pos, st['nbits']); pos += st['nbits']
        stk = []
        if st['cur'] == bc: stk.append(last_ch); code = last
        else: code = bc
        while code >= mte:
            stk.append(chs[code]); code = base[code]
        stk.append(code); last_ch = code
        st['used'] = True
        t = st['cur']; base[t] = last; chs[t] = last_ch
        nxt[t] = nxt[N+last]; nxt[N+last] = t
        get()
        out += bytes(reversed(stk))
        last = bc
    return bytes(out[:esize])
