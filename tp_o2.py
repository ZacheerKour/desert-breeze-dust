import struct,time,sys
LOG='run_o2.err'; LIVE='live.txt'
A,O=int(sys.argv[1],16),int(sys.argv[2],16)   # actor bases
dx,dy,dz=float(sys.argv[3]),float(sys.argv[4]),float(sys.argv[5]); n=int(sys.argv[6])
def h(x): return '%X'%struct.unpack('<I',struct.pack('<f',x))[0]
def f(u): return struct.unpack('<f',struct.pack('<I',u))[0]
for it in range(n):
    open(LIVE,'a').write('peek %X 3\n'%(A+0x90)); time.sleep(0.5)
    t=open(LOG,'rb').read().decode('latin1').split('\n')[-3000:]
    key='[MEM] peek %08X'%(A+0x90)
    pk=[l for l in t if l.startswith(key)]
    if not pk: continue
    v=[int(x,16) for x in pk[-1].split(':')[1].split()[:3]]
    x,y,z=f(v[0])+dx,f(v[1])+dy,f(v[2])+dz
    L=[]
    for off in (0x90,0xD0,0x4C4,0x500):
        for k,val in enumerate((x,y,z)): L.append('poke %X %s 4'%(O+off+4*k,h(val)))
    open(LIVE,'a').write('\n'.join(L)+'\n'); time.sleep(0.25)
print('done')
