import struct,sys,importlib.util
sp=importlib.util.spec_from_file_location('pd_alr','pd_alr.py'); pa=importlib.util.module_from_spec(sp); sp.loader.exec_module(pa)
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
def materials(b):
    o=0
    while u(b,o)!=0:
        t,s=u(b,o),u(b,o+4)
        if t==1:
            n=struct.unpack_from('<H',b,o+8)[0]
            return [(o+0xC+0x4c*i) for i in range(n)]
        o+=s
    return []
if __name__=='__main__':
    b=open(sys.argv[1],'rb').read(); r=pa.parse(b); ms=materials(b)
    print('textures',len(r['tex']),'materials',len(ms))
    for i,e in enumerate(ms[:int(sys.argv[2]) if len(sys.argv)>2 else 6]):
        w=struct.unpack_from('<19I',b,e); mask=struct.unpack_from('<H',b,e+0x16)[0]; idx=struct.unpack_from('<4H',b,e+0x18)
        print('mat',i,'mask',hex(mask),'tex idx',idx,'| raw',' '.join('%x'%x for x in w))
