# perfstat.py <run.err> [last N seconds]: summarise the [PERF] lines (RECOMP_PERF=1): avg fps, worst second, 1% low, worst frame.
import re,sys
f=sys.argv[1]; n=int(sys.argv[2]) if len(sys.argv)>2 else 60
L=[l for l in open(f,errors='replace') if '[PERF] flips' in l][-n:]
fps=[];mx=[];p99=[]
for l in L:
    m=re.search(r'\((\d+\.\d) fps\) interval avg ([\d.]+) p99 ([\d.]+) max ([\d.]+)',l)
    fps.append(float(m[1]));p99.append(float(m[3]));mx.append(float(m[4]))
s=sorted(fps)
print(f,'secs',len(L),'avg fps %.1f'%(sum(fps)/len(fps)),'min sec-fps %.1f'%s[0],'1%%-low sec-fps %.1f'%s[max(0,len(s)//100)],'worst p99 ms %.1f'%max(p99),'max ms %.1f'%max(mx),'secs<25: %d'%sum(1 for x in fps if x<25))
