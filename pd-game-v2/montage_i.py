# montage_i.py out.png cols in1.png in2.png ... : downscale 2x and tile
import sys,struct,zlib
exec(open('darkcnt_i.py').read().split("w,h,bpp,rows=readpng")[0])
out=sys.argv[1]; cols=int(sys.argv[2]); fs=sys.argv[3:]
tiles=[]
for f in fs:
    w,h,bpp,rows=readpng(f)
    t=[]
    for y in range(0,h-1,2):
        r=bytearray()
        for x in range(0,w-1,2): r+=rows[y][x*bpp:x*bpp+3]
        t.append(bytes(r))
    tiles.append(t)
th=len(tiles[0]); tw=len(tiles[0][0])
nr=(len(tiles)+cols-1)//cols
img=[]
for r in range(nr):
    for y in range(th):
        line=b''
        for c in range(cols):
            i=r*cols+c
            line+= tiles[i][y] if i<len(tiles) else bytes(tw)
        img.append(line)
def ch(t,b):
    c=struct.pack('>I',len(b))+t+b; return c+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
open(out,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',tw//3*cols,len(img),8,2,0,0,0))+ch(b'IDAT',zlib.compress(b''.join(b'\0'+r for r in img),6))+ch(b'IEND',b''))
