import struct,math,sys,importlib.util
sp=importlib.util.spec_from_file_location('pd_mesh','pd_mesh.py'); pm=importlib.util.module_from_spec(sp); sp.loader.exec_module(pm)
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
b,streams,meshes=pm.load(sys.argv[1])
# bone table
o=0; bones=[]
while u(b,o)!=0:
    t,s=u(b,o),u(b,o+4)
    if t==3:
        n=struct.unpack_from('<H',b,o+8)[0]
        for i in range(n):
            w=struct.unpack_from('<16I',b,o+0x10+64*i); f=struct.unpack_from('<9f',b,o+0x10+64*i)
            parent=(w[10]>>16)&0xffff
            bones.append(dict(t=f[0:3],r=f[3:6],s=f[6:9],parent=None if parent==0xffff else parent,w9=w[9]))
        break
    o+=s
print('bones',len(bones))
# check bbox: header bbox vs raw vertex range per mesh (bone-local?)
V={i:pm.verts(b,st) for i,st in enumerate(streams)}
def hdr(m):
    return struct.unpack_from('<9f',b,m['o']+0x0C)
for k,m in enumerate(meshes[:6]):
    vs=[V[m['vb']][i] for i in set(m['idx'])]
    mn=[min(v[a] for v in vs) for a in range(3)]; mx=[max(v[a] for v in vs) for a in range(3)]
    h=hdr(m)
    bone=struct.unpack_from('<H',b,m['o']+0x42)[0]
    print('mesh',k,'bone',bone,'raw bbox',[round(x,1) for x in mn],[round(x,1) for x in mx],'| hdr floats',[round(x,1) for x in h])

print('---- bone chains for mesh-attached bones')
def chain(i):
    c=[]
    while i is not None: c.append(i); i=bones[i]['parent']
    return c[::-1]
for k,m in enumerate(meshes):
    bi=struct.unpack_from('<H',b,m['o']+0x42)[0]
    ch=chain(bi)
    tsum=[sum(bones[j]['t'][a] for j in ch) for a in range(3)]
    rots=[tuple(round(x,3) for x in bones[j]['r']) for j in ch if any(abs(x)>1e-6 for x in bones[j]['r'])]
    print('mesh',k,'bone',bi,'chain',ch,'sumT',[round(x,1) for x in tsum],'nonzero rots',rots[:3],'w9',hex(bones[bi]['w9']))

print('---- raw vertex centers for offset bones')
for k in (6,7,8,10,11,12,13):
    m=meshes[k]; vs=[V[m['vb']][i] for i in set(m['idx'])]
    c=[sum(v[a] for v in vs)/len(vs) for a in range(3)]
    bi=struct.unpack_from('<H',b,m['o']+0x42)[0]; ch=chain(bi)
    tsum=[sum(bones[j]['t'][a] for j in ch) for a in range(3)]
    print('mesh',k,'bone',bi,'raw center',[round(x,1) for x in c],'bone worldT',[round(x,1) for x in tsum],'nverts',len(vs))
