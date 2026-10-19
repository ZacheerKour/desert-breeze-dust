import os
import sys,importlib.util,numpy as np
from PIL import Image,ImageDraw
sp=importlib.util.spec_from_file_location('pd_model',os.path.join(os.path.dirname(os.path.abspath(__file__)),'pd_model.py')); md=importlib.util.module_from_spec(sp); sp.loader.exec_module(md)
path,meshes_s,out=sys.argv[1],sys.argv[2],sys.argv[3]
b,r,parts=md.load_model(path)
ids=[int(x) for x in meshes_s.split(',')]
p0=parts[ids[0]]; di=p0['mat']['diffuse']; t=r['tex'][di]
img=md.pt.decode(b[r['base']+t['data']:],t['code'],t['w'],t['h']); S=t['w']
im=Image.fromarray(img,'RGBA').convert('RGB').resize((S,S))
d=ImageDraw.Draw(im,'RGBA')
for i in ids:
    p=parts[i]
    if p['mat']['diffuse']!=di: continue
    uv=p['uv']
    for a,c,e in md.tris(p['m']):
        pts=[(uv[v][0]%1*S,uv[v][1]%1*S) for v in (a,c,e)]
        d.polygon(pts,outline=(255,255,0,160))
im.save(out); print('texture',di,t['w'],t['h'],md.pt.CODES[t['code']])
