import sys
from PIL import Image, ImageDraw
V=[];F=[]
for l in open(sys.argv[1]):
    t=l.split()
    if not t: continue
    if t[0]=='v': V.append(tuple(map(float,t[1:4])))
    elif t[0]=='f': F.append(tuple(int(x)-1 for x in t[1:4]))
print('verts',len(V),'faces',len(F))
import random; random.seed(1)
step=max(1,len(F)//40000)
def render(ax1,ax2,S=520):
    P=[(v[ax1],v[ax2]) for v in V]
    mn1,mx1=min(p[0] for p in P),max(p[0] for p in P); mn2,mx2=min(p[1] for p in P),max(p[1] for p in P)
    k=(S-20)/max(mx1-mn1,mx2-mn2)
    im=Image.new('RGB',(S,S),(20,20,24)); d=ImageDraw.Draw(im)
    pt=lambda p:(10+(p[0]-mn1)*k, S-10-(p[1]-mn2)*k)
    for a,b,c in F[::step]: d.polygon([pt(P[a]),pt(P[b]),pt(P[c])],outline=(110,190,255))
    return im
a=render(0,1); b=render(0,2)
s=Image.new('RGB',(1040,520)); s.paste(a,(0,0)); s.paste(b,(520,0)); s.save(sys.argv[2])
