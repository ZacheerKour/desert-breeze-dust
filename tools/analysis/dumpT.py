import struct,sys,glob
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
def fmt(w):
    f=struct.unpack('<f',struct.pack('<I',w))[0]
    return ('%8.2f'%f) if (w!=0 and 1e-3<abs(f)<1e6) else ('%8s'%('%x'%w if w else '.'))
want={int(x) for x in sys.argv[2:]}
seen=set()
for p in sorted(glob.glob(sys.argv[1],recursive=True)):
    b=open(p,'rb').read(); o=0
    while u(b,o)!=0:
        t,s=u(b,o),u(b,o+4)
        if t in want and t not in seen and s<=208:
            seen.add(t); print('type',t,'size',s,'in',p.split('extracted')[-1],hex(o))
            for i in range(0,s//4,8): print('   +%02x'%(i*4),' '.join(fmt(x) for x in struct.unpack_from('<%dI'%min(8,s//4-i),b,o+i*4)))
        o+=s
    if seen>=want: break
