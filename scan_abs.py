import struct,sys
d=open('extracted/default.xbe','rb').read()
base=struct.unpack_from('<I',d,0x104)[0]; nsec=struct.unpack_from('<I',d,0x11C)[0]; sh=struct.unpack_from('<I',d,0x120)[0]-base
pats=[struct.pack('<I',int(x,16)) for x in sys.argv[1:]]
for i in range(nsec):
    f,v,vs,raw,rs=struct.unpack_from('<IIIII',d,sh+i*56)
    b=d[raw:raw+rs]
    for p in pats:
        o=0
        while True:
            o=b.find(p,o)
            if o<0:break
            print('%X sec%d ref %s'%(v+o,i,p[::-1].hex())); o+=1
