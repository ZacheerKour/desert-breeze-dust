import os
import glob,struct,collections,numpy as np,importlib.util
def mod(n):
    sp=importlib.util.spec_from_file_location(n,os.path.join(os.path.dirname(os.path.abspath(__file__)),n+'.py')); m=importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
md=mod('pd_model')
res=collections.Counter(); ex={}
for p in sorted(glob.glob('extracted/**/*.alr',recursive=True)+glob.glob('extracted/**/*.e',recursive=True)+glob.glob('out/unpacked/**/*.alr',recursive=True)):
    b=open(p,'rb').read()
    try: sts=md.streams(b); ms=md.pm.load(p)[2] if sts else []
    except Exception: continue
    for m in ms:
        st=sts[m['vb']]; o=m['o']; ni=m['ni']; s=struct.unpack_from('<I',b,o+4)[0]; base=struct.unpack_from('<I',b,o+0x48)[0]
        skinned=bool(st['mask']>>9&1)
        ok={}
        for start in (0x64,0x68):
            if o+start+2*ni>o+s: ok[start]=False; continue
            I=np.frombuffer(b,'<u2',ni,o+start); ok[start]=bool(I.min()==base and I.max()<base+m['nv'])
        key=('mask bit9' if skinned else 'no bit9', 'start64 ok' if ok[0x64] else '', 'start68 ok' if ok[0x68] else '')
        res[key]+=1; ex.setdefault(key,p.split('extracted')[-1])
for k,v in sorted(res.items()): print(k,v,ex[k])
