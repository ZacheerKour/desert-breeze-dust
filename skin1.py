import struct,numpy as np,importlib.util,collections
def mod(n):
    sp=importlib.util.spec_from_file_location(n,n+'.py'); m=importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
md=mod('pd_model'); sk=md.sk
P='out/unpacked/Event/1a70/npc05a.alr'
b,r,parts=md.load_model(P,False); bones=sk.read_bones(b); sts=md.streams(b)
def root_of(i):
    while bones[i]['parent'] is not None: i=bones[i]['parent']
    return i
sk_parts=[p for p in parts if root_of(p['bone'])==0]
print('skinned candidate meshes',len(sk_parts))
for p in sk_parts[:6]:
    m=p['m']; w=struct.unpack_from('<26I',b,m['o'])
    print('mesh bone',p['bone'],'mat',m['mat'],'vb',m['vb'],'nv',m['nv'],'ni',m['ni'],'size',struct.unpack_from('<I',b,m['o']+4)[0])
    print('   hdr dwords +0x08..+0x64:',' '.join('%x'%x for x in w[2:25]))
# slot 9 / 10 stats for stream 0
st=sts[0]; bits=[k for k in range(32) if st['mask']>>k&1]
raw=np.frombuffer(b,np.uint8,st['count']*st['stride'],st['off']).reshape(st['count'],st['stride'])
s9=raw[:,4*bits.index(9):4*bits.index(9)+4]; s10=raw[:,4*bits.index(10):4*bits.index(10)+4]; s6=raw[:,4*bits.index(6):4*bits.index(6)+4]
print('slot9 distinct',len({bytes(x) for x in s9}),'top',collections.Counter(bytes(x).hex() for x in s9).most_common(5))
print('slot10 distinct',len({bytes(x) for x in s10}),'top',collections.Counter(bytes(x).hex() for x in s10).most_common(6))
print('slot6 distinct',len({bytes(x) for x in s6}))
