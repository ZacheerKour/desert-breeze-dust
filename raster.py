import sys,importlib.util,numpy as np
from PIL import Image
sp=importlib.util.spec_from_file_location('pd_model','pd_model.py'); md=importlib.util.module_from_spec(sp); sp.loader.exec_module(md)
ONLY_TEX=True
def render(path,axes=(0,1),zsign=1,S=640,out='out/r.png',lit=True):
    b,r,parts=md.load_model(path,True)
    texs={}
    def tex(i):
        if i not in texs:
            t=r['tex'][i]; img=md.pt.decode(b[r['base']+t['data']:],t['code'],t['w'],t['h']); texs[i]=img
        return texs[i]
    allp=np.concatenate([md.world_pos(p) for p in parts]); a1,a2=axes; z=3-a1-a2
    mn=allp.min(0); mx=allp.max(0); k=(S-20)/max(mx[a1]-mn[a1],mx[a2]-mn[a2])
    img=np.zeros((S,S,4),np.uint8); img[:,:]=(20,20,24,255); zb=np.full((S,S),-1e9)
    import os
    only=[int(x) for x in os.environ.get('PD_ONLY','').split(',') if x]
    for pi,p in enumerate(parts):
        if only and pi not in only: continue
        W=md.world_pos(p); uv=p['uv']; mt=p['mat']; di=mt['diffuse'] if mt else None
        tx=tex(di) if (di is not None and di<len(r['tex'])) else None
        if tx is None and ONLY_TEX: continue
        X=(W[:,a1]-mn[a1])*k+10; Y=S-10-(W[:,a2]-mn[a2])*k; Z=W[:,z]*zsign
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
            zz=l0*Z[a]+l1*Z[c]+l2*Z[d]
            sub=zb[y0:y1+1,x0:x1+1]; upd=m&(zz>sub)
            if not upd.any(): continue
            if tx is not None and uv is not None:
                u_=(l0*uv[a,0]+l1*uv[c,0]+l2*uv[d,0])%1.0; v_=(l0*uv[a,1]+l1*uv[c,1]+l2*uv[d,1])%1.0
                h,w=tx.shape[:2]; col=tx[np.clip((v_*h).astype(int),0,h-1),np.clip((u_*w).astype(int),0,w-1)]
            else: col=np.array([150,150,150,255],np.uint8)
            region=img[y0:y1+1,x0:x1+1]; region[upd]=col[upd] if col.ndim==3 else col; sub[upd]=zz[upd]
    Image.fromarray(img,'RGBA').save(out)
if __name__=='__main__':
    p=sys.argv[1]; o=sys.argv[2]
    render(p,(0,1),1,out=o+'_front.png'); render(p,(0,1),-1,out=o+'_back.png')
    a=Image.open(o+'_front.png'); b=Image.open(o+'_back.png'); s=Image.new('RGBA',(1280,640)); s.paste(a,(0,0)); s.paste(b,(640,0)); s.save(o+'_both.png')
