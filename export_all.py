import glob,os,importlib.util,collections,re
sp=importlib.util.spec_from_file_location('pd_mesh','pd_mesh.py'); m=importlib.util.module_from_spec(sp); sp.loader.exec_module(m)
files=sorted(glob.glob('extracted/**/*.alr',recursive=True)+glob.glob('extracted/**/*.e',recursive=True))
ok=0; err=collections.Counter(); tris=0; nomesh=0; skipped=0; meshes=0
for p in files:
    rel=os.path.relpath(p,'extracted'); out='out/obj/'+rel.replace(chr(92),'_').replace('/','_')+'.obj'
    try:
        n,tri,bad,V,ms=m.export(p,out)
        if n==0: nomesh+=1; os.remove(out); continue
        ok+=1; tris+=tri; meshes+=n; skipped+=bad
    except Exception as e: err[type(e).__name__+': '+str(e)[:60]]+=1
print('files',len(files),'with meshes',ok,'no meshes',nomesh,'meshes',meshes,'triangles',tris,'meshes skipped',skipped)
print(dict(err))
