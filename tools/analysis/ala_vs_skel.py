import glob,struct,collections,os,importlib.util
sp=importlib.util.spec_from_file_location('pd_skel',os.path.join(os.path.dirname(os.path.abspath(__file__)),'pd_skel.py')); sk=importlib.util.module_from_spec(sp); sp.loader.exec_module(sk)
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
def recs(b):
    o=0;out=[]
    while o+8<=len(b) and u(b,o)!=0:
        s=u(b,o+4); w=struct.unpack_from('<8I',b,o); out.append((o,s,w[3]&0xffff,w[3]>>16,w)); o+=s
    return out
# embedded models: out/unpacked/Event/<n>/<name>.alr with sibling anims out/unpacked/Event/<n>/*.ala (same dir names for ak/mk)
rows=[]
for d in sorted(glob.glob('out/unpacked/Event/*'))[:200]:
    alrs=glob.glob(d+'/*.alr'); alas=glob.glob(d+'/*.ala')
    if not alrs or not alas: continue
    # find a model with skeleton
    for a in alrs:
        b=open(a,'rb').read()
        try: bones=sk.read_bones(b)
        except Exception: bones=[]
        if not bones: continue
        for al in alas[:3]:
            if os.path.basename(al).split('_')[0] != os.path.basename(a).split('.')[0]: continue
            r=recs(open(al,'rb').read()); A=[x[2] for x in r]
            rows.append((os.path.basename(d),os.path.basename(a),len(bones),os.path.basename(al),len(r),min(A),max(A),sorted(collections.Counter(x[3] for x in r).items())))
for r in rows[:14]: print(r)
print(len(rows),'matched model/clip pairs')
