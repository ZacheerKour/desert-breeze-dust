import struct,time,re,sys
LOG='run_n5.err'; LIVE='live.txt'
def h(x): return '%X'%struct.unpack('<I',struct.pack('<f',x))[0]
def f(u): return struct.unpack('<f',struct.pack('<I',u))[0]
n=int(sys.argv[1]) if len(sys.argv)>1 else 30
for it in range(n):
    open(LIVE,'a').write('peek 10E51F0 3\n'); time.sleep(0.6)
    t=open(LOG,'rb').read().decode('latin1').split('\n')
    pk=[l for l in t[-4000:] if l.startswith('[MEM] peek 010E51F0')]
    if not pk: continue
    v=[int(x,16) for x in pk[-1].split(':')[1].split()]
    x,y,z=f(v[0])+4,f(v[1]),f(v[2])+4
    B=0x10B5160; L=[]
    for off in (0x90,0xD0,0x4C4,0x500):
        for k,val in enumerate((x,y,z)): L.append('poke %X %s 4'%(B+off+4*k,h(val)))
    open(LIVE,'a').write('\n'.join(L)+'\n'); time.sleep(1.2)
print('done')
