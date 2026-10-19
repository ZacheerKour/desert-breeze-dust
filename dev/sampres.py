# resolve [SAMPLE] RIPs from a run .err via the link map. usage: sampres.py run.err [map]
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import sys,re,bisect,collections
err=sys.argv[1]; mp=sys.argv[2] if len(sys.argv)>2 else os.path.join(ROOT, 'pd-game-v2', 'build', 'pd_recomp.map')
syms=[]
for l in open(mp,errors='replace'):
    m=re.match(r'\s*0001:([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{16}) f\s+(\S+)',l)
    if m: syms.append((int(m.group(3),16)-0x140000000,m.group(2),m.group(4)))
syms.sort(); keys=[s[0] for s in syms]
def res(rva):
    i=bisect.bisect_right(keys,rva)-1
    return (syms[i][1],syms[i][2],rva-syms[i][0]) if i>=0 else ('?','',0)
byt=collections.defaultdict(collections.Counter); allc=collections.Counter(); ns=0
for l in open(err,errors='replace'):
    if not l.startswith('[SAMPLE]') or ':' not in l: continue
    ns+=1
    for tok in l.split(':',1)[1].split():
        if '=' not in tok: continue
        t,r=tok.split('=',1)
        if not r.startswith('@') and not re.fullmatch(r'[0-9A-Fa-f]+',r): continue
        n=(r,'',0) if r.startswith('@') else res(int(r,16))
        if r.startswith('@'): n=(r.split('+')[0],'',0)
        byt[t][n[0]]+=1; allc[n[0]]+=1
print('samples',ns)
print('--- all threads'); [print(c,n) for n,c in allc.most_common(25)]
for t,c in byt.items():
    print('--- thread',t,sum(c.values())); [print('  ',v,n) for n,v in c.most_common(10)]
