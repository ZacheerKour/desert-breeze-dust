import os
import glob,struct,collections,importlib.util,math
sp=importlib.util.spec_from_file_location('pd_mesh',os.path.join(os.path.dirname(os.path.abspath(__file__)),'pd_mesh.py')); pm=importlib.util.module_from_spec(sp); sp.loader.exec_module(pm)
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
files=sorted(glob.glob('extracted/**/*.alr',recursive=True)+glob.glob('extracted/**/*.e',recursive=True))
agg=collections.defaultdict(lambda:collections.defaultdict(list))  # mask -> bit -> samples
for p in files:
    b=open(p,'rb').read(); meta=u(b,0xC); o=0
    while u(b,o)!=0:
        t,s=u(b,o),u(b,o+4)
        if t==22:
            for i in range((s-12)//28):
                w=struct.unpack_from('<7I',b,o+12+28*i); stride=(w[0]>>16)&0xff; mask=w[1]; cnt=w[2]; off=meta+w[5]
                bits=[k for k in range(32) if mask>>k&1]
                for vi in range(0,cnt,max(1,cnt//200)):
                    base=off+vi*stride
                    for si,k in enumerate(bits): agg[(stride,mask)][k].append(struct.unpack_from('<I',b,base+4*si)[0])
        o+=s
def f(x): return struct.unpack('<f',struct.pack('<I',x))[0]
for key in sorted(agg):
    print('== stride',key[0],'mask',hex(key[1]))
    for k,vals in agg[key].items():
        fl=[f(x) for x in vals]; sane=[x for x in fl if x==x and (abs(x)<1e5) and (abs(x)>1e-5 or x==0)]
        white=sum(1 for x in vals if x==0xffffffff)/len(vals); zero=sum(1 for x in vals if x==0)/len(vals)
        hi=[ (x&0xffff,x>>16) for x in vals]
        u16a=[a for a,_ in hi]; u16b=[c for _,c in hi]
        print('  bit %2d: float-sane %3.0f%% [%s..%s] white %3.0f%% zero %3.0f%% | u16lo %d..%d u16hi %d..%d'%(k,100*len(sane)/len(vals),('%.2f'%min(sane)) if sane else '-',('%.2f'%max(sane)) if sane else '-',100*white,100*zero,min(u16a),max(u16a),min(u16b),max(u16b)))
