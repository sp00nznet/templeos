import sys,struct
d=open(sys.argv[1],'rb').read()
pvd=d[16*2048:17*2048]
print('vol',pvd[40:72], 'blocks',struct.unpack('<I',pvd[80:84]), 'bs',struct.unpack('<H',pvd[128:130]))
root=pvd[156:156+34]
def rec(r): l=r[0];ext=struct.unpack('<I',r[2:6])[0];sz=struct.unpack('<I',r[10:14])[0];fl=r[25];nl=r[32];return l,ext,sz,fl,r[33:33+nl]
print(rec(root))
br=d[17*2048:18*2048]; cat=struct.unpack('<I',br[71:75])[0]; print('boot cat',cat, d[cat*2048:cat*2048+64].hex())
