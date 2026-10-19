import glob,struct,os,importlib.util,collections
sp=importlib.util.spec_from_file_location('pd_skel',os.path.join(os.path.dirname(os.path.abspath(__file__)),'pd_skel.py')); sk=importlib.util.module_from_spec(sp); sp.loader.exec_module(sk)
pm=sk.pm
files=sorted(glob.glob('extracted/**/*.alr',recursive=True)+glob.glob('extracted/**/*.e',recursive=True))
nf=0; noskel=0; oob=0; meshes=0; nbones=[]; parent_bad=0; cyc=0; usedfrac=[]
for p in files:
    b,streams,ms=pm.load(p)
    if not ms: continue
    nf+=1; bones=sk.read_bones(b)
    if not bones: noskel+=1; continue
    nbones.append(len(bones))
    for i,bn in enumerate(bones):
        if bn['parent'] is not None and bn['parent']>=len(bones): parent_bad+=1
    # cycle check
    for i in range(len(bones)):
        seen=set(); j=i
        while j is not None and j<len(bones):
            if j in seen: cyc+=1; break
            seen.add(j); j=bones[j]['parent']
    used=set()
    for m in ms:
        meshes+=1; bi=struct.unpack_from('<H',b,m['o']+0x42)[0]; used.add(bi)
        if bi>=len(bones): oob+=1
    usedfrac.append(len(used)/len(bones))
print('mesh files',nf,'without skeleton',noskel,'meshes',meshes,'bone idx out of range',oob)
print('bone counts min/med/max',min(nbones),sorted(nbones)[len(nbones)//2],max(nbones),'| bad parent refs',parent_bad,'| cycles',cyc)
print('avg fraction of bones that carry a mesh %.2f'%(sum(usedfrac)/len(usedfrac)))
