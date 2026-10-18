import glob,struct,collections,importlib.util
sp=importlib.util.spec_from_file_location('pd_mesh','pd_mesh.py'); pm=importlib.util.module_from_spec(sp); sp.loader.exec_module(pm)
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
files=sorted(glob.glob('extracted/**/*.alr',recursive=True)+glob.glob('extracted/**/*.e',recursive=True))
c=collections.Counter(); ex={}; usedby=collections.Counter()
for p in files:
    b,streams,ms=pm.load(p)
    for st in streams:
        o=st['off']
        k=(st['stride'],st['vtype']); c[k]+=st['count']; ex.setdefault(k,(p,st))
print('(stride,vtype): total vertices')
for k,v in sorted(c.items()): print(' ',k,v,ex[k][0].split('extracted')[-1])
# raw fmt dwords
f=collections.Counter()
for p in files:
    b,streams,ms=pm.load(p)
    o=0
    while u(b,o)!=0:
        t,s=u(b,o),u(b,o+4)
        if t==22:
            for i in range((s-12)//28): f[struct.unpack_from('<2I',b,o+12+28*i)]+=1
        o+=s
print('fmt dwords (w0,w1):',[(hex(a),hex(b_),n) for (a,b_),n in f.most_common(12)])
