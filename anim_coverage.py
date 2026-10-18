import glob,struct,collections
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
files=sorted(glob.glob('extracted/**/*.ala',recursive=True)+glob.glob('out/unpacked/**/*.ala',recursive=True))
c=collections.Counter(); mis=collections.Counter()
for p in files:
    b=open(p,'rb').read(); o=0
    while u(b,o)!=0:
        s=u(b,o+4); w=struct.unpack_from('<8I',b,o); k=w[3]>>16; n_aux,n_rot,w7=w[4],w[5],w[7]; esz=w7>>16
        if k==7 and (w7&0xff)==7:
            ok=(32+n_aux*esz+7*n_rot+3)//4*4==s; c[('kind7','fit' if ok else 'misfit')]+=1
            if not ok: mis[(hex(w7),n_aux,n_rot,s)]+=1
        elif k==8: c[('kind8','fit' if (32+n_aux*esz+3)//4*4==s or 32+n_aux*8<=s else 'misfit')]+=1
        elif k==3: c[('kind3','3B/frame' if (32+n_aux*esz+3*n_rot+3)//4*4==s else 'other')]+=1
        else: c[('other kind %d'%k,'')]+=1
        o+=s
for k,v in sorted(c.items()): print(k,v)
print('top misfits (w7,n_aux,n_rot,size):',mis.most_common(6))
