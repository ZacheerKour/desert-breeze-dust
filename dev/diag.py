import glob,os,importlib.util,collections
sp=importlib.util.spec_from_file_location('pd_mesh',os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','tools','analysis','pd_mesh.py')); m=importlib.util.module_from_spec(sp); sp.loader.exec_module(m)
files=sorted(glob.glob('extracted/**/*.alr',recursive=True))
why=collections.Counter(); perfile={}
for p in files:
    b,streams,ms=m.load(p)
    if not ms: continue
    c=collections.Counter()
    for x in ms:
        if x['prim']!=5: c['prim=%d'%x['prim']]+=1
        elif x['vb']>=len(streams): c['vb_oob(streams=%d)'%len(streams)]+=1
        elif max(x['idx'],default=0)>=streams[x['vb']]['count']: c['idx>=count']+=1
        else: c['ok']+=1
    perfile[p]=c; why.update(c)
print(why)
bad=[(p.split('extracted')[-1],dict(c)) for p,c in perfile.items() if c.get('ok',0)==0]
print(len(perfile),'mesh files;',len(bad),'with zero ok. samples:')
for x in bad[:6]: print(' ',x)
good=[(p.split('extracted')[-1],dict(c)) for p,c in perfile.items() if c.get('ok',0)>0][:5]
print('some ok:',good)
