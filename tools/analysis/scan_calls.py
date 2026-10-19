import struct,sys
d=open('extracted/default.xbe','rb').read()
base=struct.unpack_from('<I',d,0x104)[0]; nsec=struct.unpack_from('<I',d,0x11C)[0]; sh=struct.unpack_from('<I',d,0x120)-0 if False else struct.unpack_from('<I',d,0x120)[0]-base
tgt=[int(x,16) for x in sys.argv[1:]]
for i in range(nsec):
    f,v,vs,raw,rs=struct.unpack_from('<IIIII',d,sh+i*56)
    if not (f&0x4) and not (v<0x200000): pass
    b=d[raw:raw+rs]
    for o in range(len(b)-4):
        if b[o] in (0xE8,0xE9):
            t=(v+o+5+struct.unpack_from('<i',b,o+1)[0])&0xffffffff
            if t in tgt: print('%X -> %X (%02X)'%(v+o,t,b[o]))
