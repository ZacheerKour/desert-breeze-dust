# nav.py <tag> <tx> <tz> [steps]: closed-loop walk of Alpha (real pad input only) toward world (tx, tz) using position read-back.
import struct,time,sys,math
tag=sys.argv[1]; tx,tz=float(sys.argv[2]),float(sys.argv[3]); steps=int(sys.argv[4]) if len(sys.argv)>4 else 80
A=0x1086160
LIVE='pd-game-v2/mem_live_%s.txt'%tag; LOG='pd-game-v2/run_%s.err'%tag; PAD='pd-game-v2/pad_live_%s.txt'%tag
def f(u): return struct.unpack('<f',struct.pack('<I',u))[0]
def pos():
    import re
    key='[MEM] peek %08X'%(A+0x90)
    def lines(): return [l for l in open(LOG,'rb').read().decode('latin1').split(chr(10))[-3000:] if l.startswith(key)]
    n0=len(lines()); open(LIVE,'a').write('peek %X 3'%(A+0x90)+chr(10))
    for _ in range(60):
        time.sleep(0.1)
        L=lines()
        if len(L)>n0 or _>20:
            for l in reversed(L):
                h=re.findall(r'(?<![0-9A-Fa-f])[0-9A-F]{8}(?![0-9A-Fa-f])',l.split(':',1)[1])
                if len(h)>=3: return [f(int(x,16)) for x in h[:3]]
    return None
sign=1; prev=pos(); last_err=1e9; worse=0; m=None
print('start',prev)
for i in range(steps):
    d=(tx-prev[0],tz-prev[2]); dist=math.hypot(*d)
    if dist<10: print('ARRIVED',prev,'dist',dist); break
    if m is None: cmd='lup'
    else:
        ang=math.atan2(d[1],d[0])-math.atan2(m[1],m[0]); ang=(ang+math.pi)%(2*math.pi)-math.pi
        a=math.degrees(ang)*sign
        if abs(a)<30: cmd='lup'
        elif a>0: cmd='lup+lleft' if abs(a)<100 else 'lleft'
        else: cmd='lup+lright' if abs(a)>-100 else 'lright'
        if abs(a)>150: cmd='ldown'
    open(PAD,'a').write('%s:700\n'%cmd); time.sleep(1.9)
    cur=pos()
    if cur is None: continue
    mv=(cur[0]-prev[0],cur[2]-prev[2])
    if math.hypot(*mv)>2: m=mv
    nd=math.hypot(tx-cur[0],tz-cur[2])
    worse=worse+1 if nd>last_err+1 else 0
    if worse>=3: sign=-sign; worse=0; print('flip sign')
    last_err=nd; prev=cur
    print(i,cmd,'pos %.0f %.0f %.0f'%tuple(cur),'dist %.0f'%nd,flush=True)
