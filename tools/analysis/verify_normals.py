import os
import glob,struct,itertools,importlib.util,numpy as np,collections
sp=importlib.util.spec_from_file_location('pd_model',os.path.join(os.path.dirname(os.path.abspath(__file__)),'pd_model.py')); md=importlib.util.module_from_spec(sp); sp.loader.exec_module(md)
def geo_normals(pos,T,n):
    g=np.zeros((n,3))
    for a,b,c in T:
        f=np.cross(pos[b]-pos[a],pos[c]-pos[a]); g[a]+=f; g[b]+=f; g[c]+=f
    l=np.linalg.norm(g,axis=1,keepdims=True); l[l==0]=1; return g/l
def decode(n,order,bits):
    # order: which field (0,1,2) is in low bits..., bits widths
    out=[];sh=0
    for w in bits:
        v=((n>>sh)&((1<<w)-1)).astype(np.int32); v=np.where(v>=(1<<(w-1)),v-(1<<w),v)/float((1<<(w-1))-1); out.append(v); sh+=w
    out=np.stack(out,1); res=np.zeros_like(out)
    for i,o in enumerate(order): res[:,o]=out[:,i]
    return res
files=sorted(glob.glob('extracted/**/*.alr',recursive=True)); res=collections.defaultdict(list); count=0
for p in files:
    b=open(p,'rb').read()
    try: sts=md.streams(b)
    except Exception: continue
    if not sts: continue
    meshes=md.pm.load(p)[2]
    for m in meshes[:40]:
        st=sts[m['vb']]; bits=[k for k in range(32) if st['mask']>>k&1]
        if 3 not in bits: continue
        raw=np.frombuffer(b,np.uint8,st['count']*st['stride'],st['off']).reshape(st['count'],st['stride'])
        n=raw[:,4*bits.index(3):4*bits.index(3)+4].copy().view('<u4')[:,0]
        pos=np.stack([raw[:,4*i:4*i+4].copy().view('<f4')[:,0] for i in range(3)],1)
        T=md.tris(m)
        if len(T)<20: continue
        used=sorted({i for t in T for i in t}); G=geo_normals(pos,T,st['count'])
        for name,order,bw in [('xyz 11-11-10 (low=x)',(0,1,2),(11,11,10)),('zyx 10-11-11 (low=z)',(2,1,0),(10,11,11)),('yxz',(1,0,2),(11,11,10)),('xzy',(0,2,1),(11,11,10))]:
            D=decode(n,order,bw); L=np.linalg.norm(D,axis=1)
            d=np.sum(D[used]*G[used],1)/np.maximum(L[used],1e-9)
            res[name].append((float(np.mean(d)),float(np.median(L[used]))))
        count+=1
print('meshes tested',count)
for k,v in res.items(): print('%-24s mean dot %.3f  | median |n| %.2f'%(k,np.mean([a for a,_ in v]),np.median([c for _,c in v])))

# per-mesh distribution for the best decode
vals=[a for a,_ in res['xyz 11-11-10 (low=x)']]
v=np.array(vals)
print('per-mesh mean dot: >0.9: %.0f%%  0.5..0.9: %.0f%%  0..0.5: %.0f%%  <0: %.0f%%'%(100*np.mean(v>0.9),100*np.mean((v>0.5)&(v<=0.9)),100*np.mean((v>0)&(v<=0.5)),100*np.mean(v<=0)))
