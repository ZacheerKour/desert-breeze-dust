import glob,struct,collections,numpy as np,importlib.util,random
def mod(n):
    sp=importlib.util.spec_from_file_location(n,n+'.py'); m=importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
md=mod('pd_model'); random.seed(3)
def tri_list(I,prim):
    I=[int(x) for x in I]
    if prim==5: T=[(I[j],I[j+1],I[j+2]) for j in range(0,len(I)-2,3)]
    elif prim==6: T=[(I[j],I[j+1],I[j+2]) if j%2==0 else (I[j+1],I[j],I[j+2]) for j in range(len(I)-2)]
    else: T=[(I[0],I[j],I[j+1]) for j in range(1,len(I)-1)]
    return [t for t in T if t[0]!=t[1] and t[1]!=t[2] and t[0]!=t[2]]
def mean_edge(P,T):
    if not T: return float('nan')
    T=np.array(T); a,b_,c=P[T[:,0]],P[T[:,1]],P[T[:,2]]
    return float(np.mean(np.concatenate([np.linalg.norm(a-b_,axis=1),np.linalg.norm(b_-c,axis=1),np.linalg.norm(c-a,axis=1)])))
rows=collections.defaultdict(list); files=sorted(glob.glob('extracted/**/*.alr',recursive=True)+glob.glob('extracted/**/*.e',recursive=True))
for p in files:
    b=open(p,'rb').read()
    try: sts=md.streams(b); ms=md.pm.load(p)[2] if sts else []
    except Exception: continue
    if not ms: continue
    S=[md.read_stream(b,st)[0] for st in sts]
    for m in random.sample(ms,min(len(ms),12)):
        o=m['o']; ni=m['ni']; s=struct.unpack_from('<I',b,o+4)[0]
        if o+0x68+2*ni>o+s: continue
        P=S[m['vb']]; e={}
        for start in (0x64,0x68):
            I=np.frombuffer(b,'<u2',ni,o+start)
            if I.max()>=len(P): e[start]=float('nan'); continue
            e[start]=mean_edge(P,tri_list(I,m['prim']))
        if not (np.isnan(e[0x64]) or np.isnan(e[0x68])): rows[bool(sts[m['vb']]['mask']>>9&1)].append(e)
for k,v in rows.items():
    a=np.array([x[0x64] for x in v]); c=np.array([x[0x68] for x in v])
    print('bit9=%s meshes %d | mean edge start+0x64: median %.2f | start+0x68: median %.2f | +0x68 smaller in %.0f%%'%(k,len(v),np.median(a),np.median(c),100*np.mean(c<a)))
