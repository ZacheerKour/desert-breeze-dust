import glob,struct,collections
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
f=lambda w:struct.unpack('<f',struct.pack('<I',w))[0]
files=sorted(glob.glob('extracted/**/*.alr',recursive=True)+glob.glob('extracted/**/*.e',recursive=True))
hdr=collections.Counter(); ex={}
w3=collections.Counter(); w5=collections.Counter(); w7=collections.Counter()
for p in files:
    b=open(p,'rb').read(); n=struct.unpack_from('<H',b,0x10)[0]; offs=struct.unpack_from('<%dI'%n,b,0x20)
    for r in offs:
        o=r
        while u(b,o)!=0:
            t,s=u(b,o),u(b,o+4)
            if t==5:
                w=struct.unpack_from('<10I',b,o)
                # fields: +8 float duration, +0xC u16,u16, +0x10 count, +0x14,+0x18, +0x1C
                w3[(w[3]&0xffff,w[3]>>16)]+=1; w7[hex(w[7])]+=1
                k=(w[3]&0xffff,w[3]>>16,hex(w[7]))
                hdr[k]+=1; ex.setdefault(k,(p.split('extracted')[-1],o,s,w[4],hex(w[5]),hex(w[6]),hex(w[8]),hex(w[9]),f(w[2])))
            o+=s
print('(u16 a, u16 b, w7):count  example(file,off,size,w4,w5,w6,w8,w9,w2f)')
for k,v in hdr.most_common(14): print(k,v,ex[k])
