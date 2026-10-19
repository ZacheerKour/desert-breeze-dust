import sys,struct,zlib
def readpng(p):
    d=open(p,'rb').read(); i=8; idat=b''
    while i<len(d):
        l,=struct.unpack('>I',d[i:i+4]); t=d[i+4:i+8]; b=d[i+8:i+8+l]; i+=12+l
        if t==b'IHDR': w,h,bd,ct=struct.unpack('>IIBB',b[:10])
        if t==b'IDAT': idat+=b
    raw=zlib.decompress(idat); bpp={2:3,6:4}[ct]; st=w*bpp; out=[]; prev=bytearray(st); o=0
    for y in range(h):
        f=raw[o]; line=bytearray(raw[o+1:o+1+st]); o+=1+st
        for x in range(st):
            a=line[x-bpp] if x>=bpp else 0; b=prev[x]; c=prev[x-bpp] if x>=bpp else 0
            if f==1: line[x]=(line[x]+a)&255
            elif f==2: line[x]=(line[x]+b)&255
            elif f==3: line[x]=(line[x]+(a+b)//2)&255
            elif f==4:
                pa=abs(b-c); pb=abs(a-c); pc=abs(a+b-2*c)
                pr=a if pa<=pb and pa<=pc else (b if pb<=pc else c); line[x]=(line[x]+pr)&255
        out.append(line); prev=line
    return w,h,bpp,out
w,h,bpp,rows=readpng(sys.argv[1]); x0,y0,x1,y1=map(int,sys.argv[2:6])
n=0;tot=0;s=0
for y in range(y0,y1):
    for x in range(x0,x1):
        px=rows[y][x*bpp:x*bpp+3]; l=(px[0]+px[1]+px[2])/3; s+=l; tot+=1
        if l<14: n+=1
print('dark<14: %d of %d, mean %.1f'%(n,tot,s/tot))
