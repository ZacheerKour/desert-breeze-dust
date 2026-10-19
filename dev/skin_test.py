import os
import struct,numpy as np,importlib.util,itertools
def mod(n):
    sp=importlib.util.spec_from_file_location(n,os.path.join(os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','tools','analysis'),n+'.py')); m=importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
md=mod('pd_model'); sk=md.sk; an=mod('pd_anim')
M='out/unpacked/Event/1a70/npc05a.alr'; C='out/unpacked/Event/1a70/npc05a_u-stand.ala'
b,r,parts=md.load_model(M,False); bones=sk.read_bones(b); sts=md.streams(b); clip=an.read_clip(open(C,'rb').read())
def root_of(i):
    while bones[i]['parent'] is not None: i=bones[i]['parent']
    return i
Wb=[np.array(x) for x in sk.world(bones)]
def pal_of(p):
    m=p['m']; o=m['o']; end=(0x68+2*m['ni']+3)//4*4; n=struct.unpack_from('<I',b,o+end+4)[0]
    return list(b[o+end+8:o+end+8+n]) if 0<n<64 else None
G=[p for p in parts if root_of(p['bone'])==0 and pal_of(p) is not None]
st=sts[0]; bits=[k for k in range(32) if st['mask']>>k&1]
raw=np.frombuffer(b,np.uint8,st['count']*st['stride'],st['off']).reshape(st['count'],st['stride'])
P=np.stack([raw[:,4*i:4*i+4].copy().view('<f4')[:,0] for i in range(3)],1)
s9=raw[:,4*bits.index(9):4*bits.index(9)+4].astype(float); s10=raw[:,4*bits.index(10):4*bits.index(10)+4].copy().view('<u2').astype(int)
def pose_world(frame):
    return [np.array(x) for x in sk.world(an.pose(bones,clip,frame))]
def proxy_points(Wp):
    pts=[]
    for p in parts:
        if root_of(p['bone'])!=21: continue
        pos=p['pos']; Mx=Wp[p['bone']]; pts.append(pos@Mx[:3,:3].T+Mx[:3,3])
    return np.concatenate(pts)
def nn_dist(X,Y):
    # mean nearest distance X->Y (chunked)
    d=[]
    for i in range(0,len(X),400):
        D=np.linalg.norm(X[i:i+400,None,:]-Y[None,:,:],axis=2); d.append(D.min(1))
    return float(np.concatenate(d).mean())
def skinned(Wp,mapping,wmode):
    out=[]
    for p in G:
        m=p['m']; base=struct.unpack_from('<I',b,m['o']+0x48)[0]; vs=np.arange(base,base+m['nv']); pal=pal_of(p); V=P[vs]
        res=np.zeros((len(vs),3))
        b1=s9[vs,1]/255.0; b2=s9[vs,2]/255.0
        wA,wB=(b1,b2) if wmode==0 else (b2,b1)
        for wi,(w,col) in enumerate(((wA,0),(wB,1))):
            reg=s10[vs,col]
            if mapping=='direct': bone=reg//3
            elif mapping=='pal96': k=(reg-96)//3; bone=np.array([pal[x] if 0<=x<len(pal) else -1 for x in k])
            elif mapping=='pal120': k=(reg-120)//3; bone=np.array([pal[x] if 0<=x<len(pal) else -1 for x in k])
            elif mapping=='pal0': k=reg//3; bone=np.array([pal[x] if 0<=x<len(pal) else -1 for x in k])
            for j in range(len(vs)):
                bi=int(bone[j]) if not isinstance(bone,np.ndarray) or bone.ndim==1 else -1
                if bi<0 or bi>=len(bones): 
                    res[j]+=w[j]*V[j]; continue
                T=Wp[bi]@np.linalg.inv(Wb[bi]); res[j]+=w[j]*(T[:3,:3]@V[j]+T[:3,3])
        out.append(res)
    return np.concatenate(out)
frame=30.0; Wp=pose_world(frame); proxy=proxy_points(Wp)
allV=np.concatenate([P[struct.unpack_from('<I',b,p['m']['o']+0x48)[0]:struct.unpack_from('<I',b,p['m']['o']+0x48)[0]+p['m']['nv']] for p in G])
print('meshes',len(G),'verts',len(allV),'proxy points',len(proxy))
print('baseline (unskinned bind pose): mean dist to proxy %.3f'%nn_dist(allV,proxy))
for mapping in ('direct','pal96','pal120','pal0'):
    for wmode in (0,1):
        print('%-6s wmode %d: mean dist %.3f'%(mapping,wmode,nn_dist(skinned(Wp,mapping,wmode),proxy)))

print('\n--- per mesh: baseline vs candidates (mean nearest distance to proxy body at frame 30)')
def per_mesh(Wp,mapping,wmode):
    X=skinned(Wp,mapping,wmode); res=[];o=0
    for p in G:
        n=p['m']['nv']; res.append(X[o:o+n]); o+=n
    return res
base_=[P[struct.unpack_from('<I',b,p['m']['o']+0x48)[0]:struct.unpack_from('<I',b,p['m']['o']+0x48)[0]+p['m']['nv']] for p in G]
cands={'pal120/w1':per_mesh(Wp,'pal120',1),'pal120/w0':per_mesh(Wp,'pal120',0)}
rows=[]
for i,p in enumerate(G):
    row=[p['bone'],nn_dist(base_[i],proxy)]+[nn_dist(c[i],proxy) for c in cands.values()]
    rows.append(row); print(' mesh bone %2d  pal=%s  bind %.2f | pal120/w1 %.2f | pal120/w0 %.2f'%(p['bone'],pal_of(p),row[1],row[2],row[3]))
far=[i for i,r in enumerate(rows) if r[1]>1.0]
print('meshes with bind dist > 1.0 (they should visibly move):',[G[i]['bone'] for i in far])
if far:
    for k,name in ((2,'pal120/w1'),(3,'pal120/w0')): print(' %s on those: mean %.3f vs bind %.3f'%(name,np.mean([rows[i][k] for i in far]),np.mean([rows[i][1] for i in far])))
