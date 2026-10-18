import glob,struct,collections
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
files=sorted(glob.glob('extracted/**/*.alr',recursive=True)+glob.glob('extracted/**/*.e',recursive=True))
res=collections.defaultdict(lambda:collections.Counter())
for p in files:
    b=open(p,'rb').read(); meta=u(b,0xC); o=0
    while u(b,o)!=0:
        t,s=u(b,o),u(b,o+4)
        if t==22:
            for i in range((s-12)//28):
                w=struct.unpack_from('<7I',b,o+12+28*i); stride=(w[0]>>16)&0xff; mask=w[1]; cnt=w[2]; off=meta+w[5]
                bits=[k for k in range(32) if mask>>k&1]
                step=max(1,cnt//300); idxs=range(0,cnt,step)
                for si,k in enumerate(bits):
                    vals=[struct.unpack_from('<I',b,off+vi*stride+4*si)[0] for vi in idxs]
                    key=(hex(mask),k)
                    n=len(vals)
                    res[key]['streams']+=1
                    if all((x&0xffff)<=4100 and (x>>16)<=4100 for x in vals): res[key]['uv12']+=1
                    if sum(1 for x in vals if x==0xffffffff)>0.9*n: res[key]['white']+=1
                    if all(x==0 for x in vals): res[key]['zero']+=1
                    sh=[(x&0xffff,x>>16) for x in vals]
                    if all((a<=32768 or a>=32768) for a,c in sh) and max(a for a,c in sh)<=32767 and max(c for a,c in sh)<=32767: res[key]['s15']+=1
        o+=s
for key in sorted(res,key=lambda k:(int(k[0],16),k[1])):
    c=res[key]; 
    if key[1]<4: continue
    print(key,dict(c))
