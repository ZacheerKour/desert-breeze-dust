import glob,struct,collections,math
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
files=sorted(glob.glob('extracted/**/*.ala',recursive=True)+glob.glob('out/unpacked/**/*.ala',recursive=True))
stats=collections.Counter(); bad=[]; mx=0; auxtypes=collections.Counter()
for p in files:
    b=open(p,'rb').read(); o=0
    while u(b,o)!=0:
        s=u(b,o+4); w=struct.unpack_from('<8I',b,o); a,k=w[3]&0xffff,w[3]>>16; n_aux,n_rot,w7=w[4],w[5],w[7]
        if k==7:
            stats['k7']+=1
            # try aux element sizes
            fit=None
            for name,esz in (('none',0),('scalar8',8),('vec3_16',16),('quat?20',20)):
                tot=32+(n_aux*esz if n_aux else 0)+7*n_rot
                if (tot+3)//4*4==s: fit=name; break
            auxtypes[(hex(w7),fit if n_aux else 'none')]+=1
            if fit is None: bad.append((p.split('extracted')[-1],o,s,n_aux,n_rot,hex(w7)))
        o+=s
print(dict(stats)); print('aux layout by w7:',dict(auxtypes)); print('size-formula misfits',len(bad),bad[:5])
