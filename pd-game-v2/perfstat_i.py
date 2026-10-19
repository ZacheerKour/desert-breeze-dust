# perfstat_i.py <run.err> [last N seconds]: presented frames/s and game flips/s from [PERF] lines (interp build).
import re,sys
f=sys.argv[1]; n=int(sys.argv[2]) if len(sys.argv)>2 else 60
L=[l for l in open(f,errors='replace') if '[PERF] presents' in l][-n:]
pr=[];fl=[];mx=[]
for l in L:
    m=re.search(r'presents \d+ \(([\d.]+)/s\) flips \d+ in [\d.]+s \(([\d.]+) fps\) interval avg ([\d.]+) p99 ([\d.]+) max ([\d.]+)',l)
    pr.append(float(m[1]));fl.append(float(m[2]));mx.append(float(m[5]))
s=sorted(pr)
print(f,'secs',len(L),'presented/s avg %.1f min %.1f p1 %.1f'%(sum(pr)/len(pr),s[0],s[max(0,len(s)//100)]),'| game flips/s avg %.1f'%(sum(fl)/len(fl)),'| max flip interval %.1f ms'%max(mx),'| secs presented<55: %d'%sum(1 for x in pr if x<55))
