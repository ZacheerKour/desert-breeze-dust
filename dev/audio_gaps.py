#!/usr/bin/env python3
"""Find digital-silence gaps inside live audio in an in-engine dump.
usage: audio_gaps.py dump.wav [start_sec] [end_sec] -> run-length histogram and event correlation"""
import sys, numpy as np
p=sys.argv[1]
a=np.fromfile(p,dtype='<i2',offset=44).reshape(-1,2)
sr=48000
s0=int(float(sys.argv[2])*sr) if len(sys.argv)>2 else 0
s1=int(float(sys.argv[3])*sr) if len(sys.argv)>3 else len(a)
z=(a[:,0]==0)&(a[:,1]==0)
# runs of zeros between nonzero context (>=2000 nonzero samples on each side within 4000)
runs=[]
i=s0
n=min(s1,len(a))
zz=z[:n]
d=np.diff(np.concatenate(([0],zz.astype(np.int8),[0])))
st=np.nonzero(d==1)[0]; en=np.nonzero(d==-1)[0]
for b,e in zip(st,en):
    if b<s0 or e-b<32: continue
    if b<2000 or e+2000>=n: continue
    if zz[b-2000:b].mean()>0.1 or zz[e:e+2000].mean()>0.1: continue
    runs.append((b,e-b))
print("gaps >=32 samples inside live audio:",len(runs),"in %.1fs"%((n-s0)/sr))
from collections import Counter
print("length histogram:",sorted(Counter(l for _,l in runs).items())[:20])
ev=[]
try:
    for l in open(p+'.events.txt'):
        f=l.split(); ev.append((int(f[0]),f[1],f[2]))
except Exception: pass
near=Counter()
for b,l in runs:
    best=None
    for t,k,v in ev:
        if -600<=b-t<=1200:
            best=(k); break
    near[best]+=1
print("preceding event within 1200 samples:",dict(near))
