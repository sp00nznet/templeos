"""Extract the TempleOS ISO's RedSea tree to a host directory, expanding .Z files."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import redsea, tosz
iso, out = sys.argv[1], sys.argv[2]
d = open(iso, 'rb').read()
drv, root = redsea.boot(d)
n = 0
for p, c, sz, attr in redsea.walk(d, root):
    if sz > 1 << 20: print('skip (large)', p); continue
    b = d[c*512:c*512+sz]
    if p.endswith('.Z'): b = tosz.expand(b); p = p[:-2]
    f = os.path.join(out, p); os.makedirs(os.path.dirname(f) or out, exist_ok=True)
    open(f, 'wb').write(b); n += 1
print(n, 'files ->', out)
