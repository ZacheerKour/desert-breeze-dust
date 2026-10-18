import glob,struct,collections,numpy as np
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
files=sorted(glob.glob('extracted/**/*.ala',recursive=True)+glob.glob('out/unpacked/**/*.ala',recursive=True))
norms=[]; comps=[]; frames_ok=0; frames_bad=0; maxdur=0; n=0; mono=0; nonmono=0
for p in files[::3]:
    b=open(p,'rb').read(); o=0
    while u(b,o)!=0:
        s=u(b,o+4); w=struct.unpack_from('<8I',b,o); k=w[3]>>16
        if k==7 and (w[7]&0xff)==7:
            n_aux,n_rot,esz=w[4],w[5],w[7]>>16
            if n_aux==0 or True:
                base=o+32+(n_aux*esz if n_aux else 0)
                if (32+(n_aux*esz if n_aux else 0)+7*n_rot+3)//4*4==s and n_rot>2:
                    raw=np.frombuffer(b,np.uint8,7*n_rot,base).reshape(n_rot,7)
                    fr=raw[:,0].astype(int)
                    xyz=np.stack([raw[:,1:3].copy().view('<i2')[:,0],raw[:,3:5].copy().view('<i2')[:,0],raw[:,5:7].copy().view('<i2')[:,0]],1).astype(np.float64)
                    if np.all(np.diff(fr)>0): mono+=1
                    else: nonmono+=1
                    maxdur=max(maxdur,struct.unpack('<f',struct.pack('<I',w[2]))[0])
                    norms.append(np.linalg.norm(xyz,axis=1)/32767.0); comps.append(np.abs(xyz).max())
                    n+=1
        o+=s
N=np.concatenate(norms)
print('rot tracks',n,'keys',len(N),'| frame byte strictly increasing in',mono,'tracks, not in',nonmono)
print('max duration among them',maxdur)
print('norm of s16 triplet /32767: max %.3f  p99 %.3f  median %.3f  frac>1.0: %.4f  frac>0.9: %.4f'%(N.max(),np.percentile(N,99),np.median(N),(N>1).mean(),(N>0.9).mean()))
print('max |component| raw',int(max(comps)))
