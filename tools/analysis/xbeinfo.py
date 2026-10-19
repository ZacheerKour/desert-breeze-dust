import struct,sys
d=open(sys.argv[1],'rb').read()
assert d[:4]==b'XBEH'
u=lambda o:struct.unpack_from('<I',d,o)[0]
base=u(0x104); soh=u(0x108); soi=u(0x10C); cert=u(0x118); nsec=u(0x11C); sec=u(0x120)
entry=u(0x128); kth=u(0x158)
print('base 0x%X  size_of_image 0x%X  headers 0x%X  sections %d'%(base,soi,soh,nsec))
co=cert-base
print('title:',d[co+0x0C:co+0x0C+80].decode('utf-16le').split('\0')[0],' titleid 0x%X'%u(co+8))
for k,nm in ((0xA8FC57AB,'retail'),(0x94859D4B,'debug')):
    e=entry^k
    if base<=e<base+soi: print('entry (%s key) = 0x%X'%(nm,e))
for k,nm in ((0x5B6D40B6,'retail'),(0xEFB1F152,'debug')):
    t=kth^k
    if base<=t<base+soi: print('kernel thunk (%s key) = 0x%X'%(nm,t))
so=sec-base
for i in range(nsec):
    fl,va,vs,ra,rs,na=struct.unpack_from('<6I',d,so+i*56)
    nm=d[na-base:na-base+12].split(b'\0')[0].decode(errors='replace')
    print('%-10s va 0x%08X vsize 0x%07X rawsize 0x%07X flags 0x%X'%(nm,va,vs,rs,fl))
