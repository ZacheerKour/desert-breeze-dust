# bmp2png_i.py in.bmp out.png : 24-bit BMP (bottom-up) -> PNG, pure python
import sys,struct,zlib
d=open(sys.argv[1],'rb').read()
off=struct.unpack_from('<I',d,10)[0]; w,h=struct.unpack_from('<ii',d,18)
row=(w*3+3)&~3
raw=bytearray()
for y in range(h):
    r=d[off+(h-1-y)*row: off+(h-1-y)*row+w*3]
    px=bytearray(r)
    for i in range(0,len(px),3): px[i],px[i+2]=px[i+2],px[i]
    raw+=b'\0'+px
def ch(t,b): 
    c=struct.pack('>I',len(b))+t+b; return c+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
open(sys.argv[2],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(bytes(raw),6))+ch(b'IEND',b''))
