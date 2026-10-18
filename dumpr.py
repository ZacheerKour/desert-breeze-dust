import struct,sys
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
def fm(w):
    f=struct.unpack('<f',struct.pack('<I',w))[0]
    return ('%9.3f'%f) if (w!=0 and 1e-4<abs(f)<1e7) else ('%9s'%('%x'%w if w else '.'))
b=open(sys.argv[1],'rb').read(); want=[int(x) for x in sys.argv[2].split(',')]; maxw=int(sys.argv[3]) if len(sys.argv)>3 else 48
o=0;i=0
while u(b,o)!=0:
    s=u(b,o+4)
    if i in want:
        print('--- rec',i,'size',s,'at',hex(o),'type',u(b,o))
        n=min(s//4,maxw)
        for j in range(0,n,8): print('  +%03x'%(j*4),' '.join(fm(x) for x in struct.unpack_from('<%dI'%min(8,n-j),b,o+j*4)))
    o+=s;i+=1
