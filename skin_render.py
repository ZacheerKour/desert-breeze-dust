import sys,struct,numpy as np,importlib.util
from PIL import Image
def mod(n):
    sp=importlib.util.spec_from_file_location(n,n+'.py'); m=importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
st_=mod('skin_test'); rp=mod('render_pose'); md=st_.md; an=st_.an; sk=st_.sk
b,bones,parts,G,P,s9,s10=st_.b,st_.bones,st_.parts,st_.G,st_.P,st_.s9,st_.s10
def skin_positions(Wp,mapping='pal120',wmode=1):
    out={}
    for p in G:
        m=p['m']; base=struct.unpack_from('<I',b,m['o']+0x48)[0]; vs=np.arange(base,base+m['nv']); pal=st_.pal_of(p); V=P[vs]; res=np.zeros((len(vs),3))
        b1=s9[vs,1]/255.0; b2=s9[vs,2]/255.0; wA,wB=(b1,b2) if wmode==0 else (b2,b1)
        for w,col in ((wA,0),(wB,1)):
            k=(s10[vs,col]-120)//3
            for j in range(len(vs)):
                bi=pal[k[j]] if 0<=k[j]<len(pal) else -1
                if bi<0: res[j]+=w[j]*V[j]; continue
                T=Wp[bi]@np.linalg.inv(st_.Wb[bi]); res[j]+=w[j]*(T[:3,:3]@V[j]+T[:3,3])
        out[id(p)]=(vs,res)
    return out
def render_frame(frame,skin=True,S=380,bounds=None,show='both'):
    Wp=st_.pose_world(frame) if frame is not None else st_.Wb
    sp_=skin_positions(Wp) if (skin and frame is not None) else None
    # build per-part world vertex arrays
    verts=[]; keep=[]
    for p in parts:
        grp=0 if p in G else (21 if st_.root_of(p['bone'])==21 else -1)
        if grp==0 and show in('both','tex'):
            M=Wp[p['bone']]
            if sp_ is not None:
                vs,res=sp_[id(p)]; V=p['pos'].copy(); V[vs]=res; W=V   # skinned positions already in model space
            else: W=p['pos']@M[:3,:3].T+M[:3,3]
            verts.append(W); keep.append(p)
        elif grp==21 and show in('both','proxy'):
            M=Wp[p['bone']]; verts.append(p['pos']@M[:3,:3].T+M[:3,3]); keep.append(p)
    return keep,verts
def raster(keep,verts,tex_r,S=380,bounds=None):
    allp=np.concatenate(verts); 
    if bounds is None: bounds=(allp.min(0),allp.max(0))
    mn,mx=bounds; a1,a2=0,1; k=(S-20)/max(mx[a1]-mn[a1],mx[a2]-mn[a2])
    img=np.zeros((S,S,4),np.uint8); img[:]=(24,24,30,255); zb=np.full((S,S),-1e9); texs={}
    for p,Wp_ in zip(keep,verts):
        mt=p['mat']; di=mt['diffuse'] if mt else None; istex=(p in G)
        tx=None
        if istex and di is not None and di<len(tex_r['tex']):
            if di not in texs: t=tex_r['tex'][di]; texs[di]=md.pt.decode(b[tex_r['base']+t['data']:],t['code'],t['w'],t['h'])
            tx=texs[di]
        uv=p['uv']; X=(Wp_[:,a1]-mn[a1])*k+10; Y=S-10-(Wp_[:,a2]-mn[a2])*k; Z=Wp_[:,2]
        for a,c,d in md.tris(p['m']):
            xs=X[[a,c,d]]; ys=Y[[a,c,d]]
            x0,x1=int(max(0,np.floor(xs.min()))),int(min(S-1,np.ceil(xs.max()))); y0,y1=int(max(0,np.floor(ys.min()))),int(min(S-1,np.ceil(ys.max())))
            if x1<x0 or y1<y0: continue
            gx,gy=np.meshgrid(np.arange(x0,x1+1)+0.5,np.arange(y0,y1+1)+0.5)
            den=(ys[1]-ys[2])*(xs[0]-xs[2])+(xs[2]-xs[1])*(ys[0]-ys[2])
            if abs(den)<1e-9: continue
            l0=((ys[1]-ys[2])*(gx-xs[2])+(xs[2]-xs[1])*(gy-ys[2]))/den; l1=((ys[2]-ys[0])*(gx-xs[2])+(xs[0]-xs[2])*(gy-ys[2]))/den; l2=1-l0-l1
            m=(l0>=-1e-4)&(l1>=-1e-4)&(l2>=-1e-4)
            if not m.any(): continue
            zz=l0*Z[a]+l1*Z[c]+l2*Z[d]; sub=zb[y0:y1+1,x0:x1+1]; upd=m&(zz>sub)
            if not upd.any(): continue
            if tx is not None and uv is not None:
                uu=(l0*uv[a,0]+l1*uv[c,0]+l2*uv[d,0])%1.0; vv=(l0*uv[a,1]+l1*uv[c,1]+l2*uv[d,1])%1.0
                h,w=tx.shape[:2]; col=tx[np.clip((vv*h).astype(int),0,h-1),np.clip((uu*w).astype(int),0,w-1)]
            else: col=np.array([90,95,110,255] if not istex else [200,120,120,255],np.uint8)
            img[y0:y1+1,x0:x1+1][upd]=col[upd] if col.ndim==3 else col; sub[upd]=zz[upd]
    return img,bounds
if __name__=='__main__':
    r=md.pa.parse(b); tiles=[]; bounds=None
    for label,frame,skin in (('bind',None,False),('f30 unskinned',30.0,False),('f30 skinned',30.0,True),('f75 skinned',75.0,True)):
        keep,verts=render_frame(frame,skin,show='both'); img,bounds=raster(keep,verts,r,bounds=bounds); tiles.append(img); print(label,'done')
    Image.fromarray(np.concatenate(tiles,1),'RGBA').save('out/skin_compare.png'); print('saved out/skin_compare.png')
