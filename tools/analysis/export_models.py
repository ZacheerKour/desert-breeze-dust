import glob,os,time,collections,importlib.util
sp=importlib.util.spec_from_file_location('pd_model',os.path.join(os.path.dirname(os.path.abspath(__file__)),'pd_model.py')); md=importlib.util.module_from_spec(sp); sp.loader.exec_module(md)
files=sorted(glob.glob('extracted/**/*.alr',recursive=True)+glob.glob('extracted/**/*.e',recursive=True))
ok=0; tris=0; texs=0; err=collections.Counter(); t0=time.time(); skipped=0
for p in files:
    b=open(p,'rb').read()
    if not md.streams(b): skipped+=1; continue
    rel=os.path.relpath(p,'extracted'); d=os.path.join('out','models',os.path.splitext(rel)[0].replace(chr(92),'_').replace('/','_'))
    try:
        n,nt,nx=md.export(p,d,True); ok+=1; tris+=nt; texs+=nx
    except Exception as e: err[type(e).__name__+': '+str(e)[:70]]+=1
print('files with vertex streams',ok,'| no streams (skipped)',skipped,'| triangles',tris,'| diffuse textures written',texs,'| %.0fs'%(time.time()-t0))
print('errors',dict(err))
