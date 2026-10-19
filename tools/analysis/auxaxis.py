import glob,struct,collections,os,math,importlib.util
sp=importlib.util.spec_from_file_location('pd_skel',os.path.join(os.path.dirname(os.path.abspath(__file__)),'pd_skel.py')); sk=importlib.util.module_from_spec(sp); sp.loader.exec_module(sk)
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
fl=lambda w:struct.unpack('<f',struct.pack('<I',w))[0]
res=collections.defaultdict(collections.Counter); seen=set()
for d in sorted(glob.glob('out/unpacked/Event/*')):
    for a in glob.glob(d+'/*.alr'):
        try: bones=sk.read_bones(open(a,'rb').read())
        except Exception: continue
        if not bones: continue
        model=os.path.basename(a).split('.')[0]
        for al in glob.glob(d+'/'+model+'_*.ala'):
            key=(model,os.path.basename(al))
            if key in seen: continue
            seen.add(key)
            b=open(al,'rb').read(); o=0
            while u(b,o)!=0:
                s=u(b,o+4); w=struct.unpack_from('<8I',b,o); bi,k=w[3]&0xffff,w[3]>>16
                if k==7 and w[4]>0 and bi<len(bones):
                    esz=w[7]>>16; mid=(w[7]>>8)&0xff; t=bones[bi]['t']
                    if esz==8: # (time, scalar)
                        v=fl(struct.unpack_from('<I',b,o+32+4)[0]); ax=[i for i in range(3) if abs(t[i]-v)<1e-3 and abs(v)>1e-3]
                        res[('scalar8',hex(mid))][tuple(ax)]+=1
                    elif esz==12:
                        v=struct.unpack_from('<3f',b,o+32); res[('vec3_12',hex(mid))][tuple(round(abs(v[i]-t[i])<1e-3) for i in range(3))]+=1
                    elif esz==16:
                        v=struct.unpack_from('<4f',b,o+32); res[('vec3t_16',hex(mid))][tuple(round(abs(v[1+i]-t[i])<0.5) for i in range(3))]+=1
                o+=s
for k in sorted(res): print(k,dict(res[k].most_common(5)))
