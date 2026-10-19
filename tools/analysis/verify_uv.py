import os
import glob,importlib.util,numpy as np,collections
sp=importlib.util.spec_from_file_location('pd_model',os.path.join(os.path.dirname(os.path.abspath(__file__)),'pd_model.py')); md=importlib.util.module_from_spec(sp); sp.loader.exec_module(md)
rng=np.random.default_rng(1)
def cv(pos,uv,T):
    e=[];
    for a,b,c in T:
        for x,y in ((a,b),(b,c),(c,a)):
            d3=np.linalg.norm(pos[x]-pos[y]); du=np.linalg.norm(uv[x]-uv[y])
            if d3>1e-6: e.append(du/d3)
    e=np.array(e); 
    return float(np.std(np.log(e[e>1e-9]))) if (e>1e-9).sum()>10 else None
res=collections.defaultdict(list); n=0
for p in sorted(glob.glob('extracted/**/*.alr',recursive=True)):
    b=open(p,'rb').read(); sts=md.streams(b)
    if not sts: continue
    for m in md.pm.load(p)[2][:25]:
        T=md.tris(m)
        if len(T)<30: continue
        st=sts[m['vb']]; bits=[k for k in range(32) if st['mask']>>k&1]
        if 6 not in bits: continue
        raw=np.frombuffer(b,np.uint8,st['count']*st['stride'],st['off']).reshape(st['count'],st['stride'])
        pos=np.stack([raw[:,4*i:4*i+4].copy().view('<f4')[:,0] for i in range(3)],1)
        def uvof(k):
            s=raw[:,4*bits.index(k):4*bits.index(k)+4].copy().view('<u4')[:,0]
            return np.stack([(s&0xffff),(s>>16)],1).astype(np.float64)/4096
        r6=cv(pos,uvof(6),T)
        if r6 is None: continue
        res['bit6 UV (decoded)'].append(r6)
        sh=uvof(6).copy(); rng.shuffle(sh); c=cv(pos,sh,T); res['control: shuffled UVs'].append(c)
        for k in bits:
            if k in (3,7,9,10,12,13,20): 
                c=cv(pos,uvof(k),T)
                if c is not None: res['slot bit %d as UV'%k].append(c)
        n+=1
print('meshes',n,'| lower = UV edge length tracks 3D edge length more consistently (std of log ratio)')
for k,v in sorted(res.items(),key=lambda x:np.median(x[1])): print('  %-26s median %.2f  (n=%d)'%(k,np.median(v),len(v)))
