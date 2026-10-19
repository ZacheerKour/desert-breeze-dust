import os
import struct,numpy as np,importlib.util,collections,glob
def mod(n):
    sp=importlib.util.spec_from_file_location(n,os.path.join(os.path.dirname(os.path.abspath(__file__)),n+'.py')); m=importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
md=mod('pd_model'); sk=md.sk
P='out/unpacked/Event/1a70/npc05a.alr'
b,r,parts=md.load_model(P,False); sts=md.streams(b)
print('mesh: base(+0x48) nv ni | idx min..max | relative(max<nv)? absolute(min>=base,max<base+nv)?')
for p in parts[:24]:
    m=p['m']; base=struct.unpack_from('<I',b,m['o']+0x48)[0]; I=np.array(m['idx'])
    print(' bone',p['bone'],'base',base,'nv',m['nv'],'ni',m['ni'],'| idx',I.min(),'..',I.max(),'| rel',I.max()<m['nv'],'abs',(I.min()>=base and I.max()<base+m['nv']))
# global test across all mesh files: how often base != 0, and which convention holds
cnt=collections.Counter()
for pth in sorted(glob.glob('extracted/**/*.alr',recursive=True)):
    bb=open(pth,'rb').read()
    try: ms=md.pm.load(pth)[2]
    except Exception: continue
    for m in ms:
        base=struct.unpack_from('<I',bb,m['o']+0x48)[0]; I=np.array(m['idx'])
        if base==0: cnt['base0']+=1
        else: cnt['base!=0 rel' if I.max()<m['nv'] else ('base!=0 abs' if (I.min()>=base and I.max()<base+m['nv']+1) else 'base!=0 other')]+=1
print(dict(cnt))
