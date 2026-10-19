# interp_viz_i.py <prefix> <n> <out.png>: strip: a | i | b (downscaled 2x) and below: |b-a|*6 | |i-avg(a,b)|*6
import sys,struct,zlib
def load(p):
    d=open(p,'rb').read(); off=struct.unpack_from('<I',d,10)[0]; w,h=struct.unpack_from('<ii',d,18); row=(w*3+3)&~3
    return w,h,[d[off+(h-1-y)*row: off+(h-1-y)*row+w*3] for y in range(h)]
pre,n,out=sys.argv[1:4]
w,h,A=load(f'{pre}_{n}_a.bmp'); _,_,I=load(f'{pre}_{n}_i.bmp'); _,_,B=load(f'{pre}_{n}_b.bmp')
# crop: central 60% to keep size reasonable, step 2 downscale
x0,x1=int(w*0.15),int(w*0.85); y0,y1=int(h*0.1),int(h*0.9)
def view(R,f=lambda r,y:r):
    rows=[]
    for y in range(y0,y1,2):
        r=f(R[y],y); rows.append(bytes(r[x*3+2-c] for x in range(x0,x1,2) for c in range(3)))  # BGR->RGB
    return rows
def diff(P,Q,k=6):
    rows=[]
    for y in range(y0,y1,2):
        p,q=P[y],Q[y]; rows.append(bytes(min(255,abs(p[x*3+c]-q[x*3+c])*k) for x in range(x0,x1,2) for c in range(3)))
    return rows
AVG=[bytes((a+b)//2 for a,b in zip(A[y],B[y])) for y in range(h)]
va,vi,vb=view(A),view(I),view(B)
d1,d2=diff(A,B),diff(I,AVG)
rows=[a+i+b for a,i,b in zip(va,vi,vb)]+[x+y+bytes(len(x)) for x,y in zip(d1,d2)]
W=len(va[0])//3*3; H=len(rows)
raw=b''.join(b'\0'+r for r in rows)
def ch(t,b):
    c=struct.pack('>I',len(b))+t+b; return c+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
open(out,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',len(rows[0])//3,H,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw,6))+ch(b'IEND',b''))
