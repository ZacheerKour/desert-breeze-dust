import struct,time,sys,math
tag=sys.argv[1]; secs=float(sys.argv[2]); LIVE='pd-game-v2/mem_live_%s.txt'%tag; LOG='pd-game-v2/run_%s.err'%tag
A,O=int(sys.argv[3],16),int(sys.argv[4],16)
def f(u): return struct.unpack('<f',struct.pack('<I',u))[0]
t0=time.time()
while time.time()-t0<secs:
    open(LIVE,'a').write('peek %X 3\npeek %X 3\n'%(A+0x90,O+0x90)); time.sleep(1.0)
L=open(LOG,'rb').read().decode('latin1').split('\n')[-20000:]
a=[l for l in L if l.startswith('[MEM] peek %08X'%(A+0x90))]; o=[l for l in L if l.startswith('[MEM] peek %08X'%(O+0x90))]
for x,y in zip(a,o):
    pa=[f(int(v,16)) for v in x.split(":")[1].split()[:3]]; po=[f(int(v,16)) for v in y.split(":")[1].split()[:3]]
    print('A %8.1f %8.1f %8.1f  O %8.1f %8.1f %8.1f  d=%.1f'%(*pa,*po,math.dist(pa,po)))
