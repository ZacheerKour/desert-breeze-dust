import os
import sys,importlib.util,numpy as np
from PIL import Image
def mod(n):
    sp=importlib.util.spec_from_file_location(n,os.path.join(os.path.dirname(os.path.abspath(__file__)),n+'.py')); m=importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
rp=mod('render_pose'); md=rp.md; an=rp.an; sk=rp.sk
model,clipf,out=sys.argv[1],sys.argv[2],sys.argv[3]; frames=[float(x) for x in sys.argv[4].split(',')]
clip=an.read_clip(open(clipf,'rb').read())
b,r,parts=md.load_model(model,False); bones=sk.read_bones(b)
def root_of(i):
    while bones[i]['parent'] is not None: i=bones[i]['parent']
    return i
orig=md.load_model
def make(group):
    def lm(p,posed=False):
        b_,r_,ps=orig(p,posed); return b_,r_,[x for x in ps if root_of(x['bone'])==group]
    return lm
rows=[]
for group in sorted({root_of(p['bone']) for p in parts}):
    md.load_model=make(group); ims=[]; bounds=None
    for f in [None]+frames:
        im,bounds=rp.render_frame(model,None if f is None else clip,0 if f is None else f,S=320,bounds=bounds,only_tex=False); ims.append(im)
    print('group root',group,'parts',sum(1 for x in parts if root_of(x['bone'])==group))
    rows.append(np.concatenate(ims,1))
Image.fromarray(np.concatenate(rows,0),'RGBA').save(out); print('saved',out,'(each row = one root group; columns = bind, then frames',frames,')')
