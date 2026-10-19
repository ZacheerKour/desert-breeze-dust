import struct,time,sys,re
LOG=sys.argv[1]; LIVE='live.txt'
A,O=int(sys.argv[2],16),int(sys.argv[3],16); dx,dy,dz=[float(x) for x in sys.argv[4:7]]; secs=float(sys.argv[7]); press=len(sys.argv)>8
def h(x): return '%X'%struct.unpack('<I',struct.pack('<f',x))[0]
def f(u): return struct.unpack('<f',struct.pack('<I',u))[0]
open(LIVE,'a').write('peek %X 3\n'%(A+0x90)); time.sleep(0.7)
t=open(LOG,'rb').read().decode('latin1').split('\n')[-4000:]
pk=[l for l in t if l.startswith('[MEM] peek %08X'%(A+0x90))]
v=[int(x,16) for x in pk[-1].split(':')[1].split()[:3]]
x,y,z=f(v[0])+dx,f(v[1])+dy,f(v[2])+dz
print('alpha',[f(a) for a in v],'-> ommato',x,y,z)
L=[]
for off in (0x90,0xD0,0x4C4,0x500):
    for k,val in enumerate((x,y,z)): L.append('poke %X %s 4'%(O+off+4*k,h(val)))
t0=time.time(); n=0
while time.time()-t0<secs:
    open(LIVE,'a').write('\n'.join(L)+'\n'); n+=1
    if press and n==3: open('pad_live.txt','a').write('x:300\n')
    time.sleep(0.05)
print('batches',n)
