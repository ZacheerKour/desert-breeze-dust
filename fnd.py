import re,sys
L=open(sys.argv[1],'rb').read().decode('latin1').split('\n')
def parse(i): return {int(a,16):int(b) for a,b in re.findall(r'([0-9A-F]{6})=(\d+)',L[i-1])}
cast=[parse(int(x)) for x in sys.argv[2].split(',')]; idle=[parse(int(x)) for x in sys.argv[3].split(',')]
ks=set().union(*cast)
res=[]
for k in ks:
    c=min(w.get(k,0) for w in cast); i=max(w.get(k,0) for w in idle)
    if c>0 and c>i*3+20: res.append((k,c,i))
res.sort(key=lambda x:-x[1])
print(len(res)); print(' '.join('%06X:%d/%d'%x for x in res[:120]))
