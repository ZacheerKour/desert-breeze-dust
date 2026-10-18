import glob,collections,importlib.util,time,os
import numpy as np
from PIL import Image
def mod(n):
    sp=importlib.util.spec_from_file_location(n,n+'.py'); m=importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
pa,pt=mod('pd_alr'),mod('pd_tex')
files=sorted(glob.glob('extracted/**/*.alr',recursive=True)+glob.glob('extracted/**/*.e',recursive=True))
ok=collections.Counter(); fail=collections.Counter(); samples=collections.defaultdict(list); t0=time.time(); oob=0
for p in files:
    b=open(p,'rb').read(); r=pa.parse(b)
    for t in r['tex']:
        k=pt.CODES.get(t['code'],hex(t['code']))
        if r['base']+t['data']>len(b): oob+=1; fail[k]+=1; continue
        img=pt.decode(b[r['base']+t['data']:],t['code'],t['w'],t['h'])
        if img is None: fail[k]+=1
        else:
            ok[k]+=1
            if len(samples[k])<4 and t['w']>=32 and t['h']>=32 and k not in('DXT1','DXT3','DXT5'): samples[k].append((p.split('extracted')[-1],t['w'],t['h'],img))
print('decoded in %.1fs'%(time.time()-t0)); print('ok',dict(ok)); print('fail',dict(fail),'oob',oob)
S=128; keys=sorted(samples); sheet=Image.new('RGBA',(4*S,max(1,len(keys))*S),(70,70,70,255))
for r_,k in enumerate(keys):
    for c_,(p,w,h,img) in enumerate(samples[k]):
        im=Image.fromarray(img,'RGBA'); im.thumbnail((S,S)); sheet.paste(im,(c_*S,r_*S),im)
    print(r_,k,[(p,w,h) for p,w,h,_ in samples[k]])
sheet.save('out/sheet_nondxt.png')
