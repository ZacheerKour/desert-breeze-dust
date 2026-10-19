import struct,sys
from capstone import Cs,CS_ARCH_X86,CS_MODE_32
d=open('extracted/default.xbe','rb').read()
base=struct.unpack_from('<I',d,0x104)[0]; nsec=struct.unpack_from('<I',d,0x11C)[0]; sh=struct.unpack_from('<I',d,0x120)[0]-base
def rd(va,n):
    for i in range(nsec):
        f,v,vs,raw,rs=struct.unpack_from('<IIIII',d,sh+i*56)
        if v<=va<v+rs: return d[raw+va-v:raw+va-v+n]
md=Cs(CS_ARCH_X86,CS_MODE_32)
va=int(sys.argv[1],16); n=int(sys.argv[2],16)
for i in md.disasm(rd(va,n),va): print('%08X  %s %s'%(i.address,i.mnemonic,i.op_str))
