import sys
def load(p): return [tuple(map(float,l.split()[1:4])) for l in open(p) if l.startswith('v ')]
def score(V,tol=0.75):
    g={}
    for v in V: g.setdefault((round(v[0]/tol),round(v[1]/tol),round(v[2]/tol)),[]).append(v)
    hit=0
    for x,y,z in V:
        k=(round(-x/tol),round(y/tol),round(z/tol)); found=False
        for dx in (-1,0,1):
            for dy in (-1,0,1):
                for dz in (-1,0,1):
                    for w in g.get((k[0]+dx,k[1]+dy,k[2]+dz),()):
                        if abs(w[0]+x)<tol and abs(w[1]-y)<tol and abs(w[2]-z)<tol: found=True;break
                    if found:break
                if found:break
            if found:break
        hit+=found
    return hit/len(V)
for p in sys.argv[1:]:
    V=load(p); print(p,'verts',len(V),'mirror-symmetric fraction %.3f'%score(V))
