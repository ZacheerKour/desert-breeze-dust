import re,sys,bisect,collections
mp=open('pd-game-v2/build/pd_recomp.map',encoding='utf-8',errors='replace').read().splitlines()
m=re.search(r'Preferred load address is ([0-9a-fA-F]+)',"\n".join(mp)); base=int(m.group(1),16)
syms=[]
for l in mp:
    g=re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8,16})\s',l)
    if g: syms.append((int(g.group(2),16)-base,g.group(1)))
syms.sort(); keys=[s[0] for s in syms]
c=collections.Counter()
for l in open(sys.argv[1],encoding='utf-8',errors='replace'):
    if l.startswith('[SAMPLE]'):
        for tok in l.split()[2:]:
            if '=' not in tok: continue
            tid,rip=tok.split('=')
            if not rip: continue
            try: r=int(rip,16)
            except ValueError: continue
            j=bisect.bisect_right(keys,r)-1
            c[(tid,syms[j][1] if j>=0 else '?')]+=1
for (tid,n),k in c.most_common(25): print(k,tid,n)
