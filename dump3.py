import struct,sys
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
b=open(sys.argv[1],'rb').read(); off=int(sys.argv[2],16)
t,s=u(b,off),u(b,off+4); n=struct.unpack_from('<H',b,off+8)[0]; fl=b[off+10]
print('type',t,'size',s,'count',n,'flags',fl,'hdr dw3',hex(u(b,off+12)))
def fmt(w):
    f=struct.unpack('<f',struct.pack('<I',w))[0]
    return ('%9.3f'%f) if (w!=0 and 1e-4<abs(f)<1e6) else ('%9s'%('%x'%w if w else '0'))
for i in range(min(n,int(sys.argv[3]) if len(sys.argv)>3 else 12)):
    w=struct.unpack_from('<16I',b,off+0x10+64*i)
    print('%2d'%i,' '.join(fmt(x) for x in w))
tail=off+0x10+64*n
print('tail starts',hex(tail),'remaining',off+s-tail)
if off+s-tail>0:
    for i in range(0,min(off+s-tail,256),32):
        w=struct.unpack_from('<8I',b,tail+i); print('  %04x'%i,' '.join(fmt(x) for x in w))
