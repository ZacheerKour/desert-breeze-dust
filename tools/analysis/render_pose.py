import os
import sys,struct,importlib.util,numpy as np
from PIL import Image
def mod(n,f=None):
    sp=importlib.util.spec_from_file_location(n,f or os.path.join(os.path.dirname(os.path.abspath(__file__)),n+'.py')); m=importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
md=mod('pd_model'); an=mod('pd_anim'); sk=md.sk
def render_frame(path,clip,frame,S=360,axes=(0,1),bounds=None,only_tex=True):
    b,r,parts=md.load_model(path,False); bones=sk.read_bones(b)
    pb=an.pose(bones,clip,frame) if clip is not None else bones
    W=sk.world(pb); texs={}
    def tex(i):
        if i not in texs:
            t=r['tex'][i]; texs[i]=md.pt.decode(b[r['base']+t['data']:],t['code'],t['w'],t['h'])
        return texs[i]
    verts=[]
    for p in parts:
        P=p['pos']; M=np.array(W[p['bone']],np.float64) if p['bone']<len(W) else np.eye(4)
        verts.append(P@M[:3,:3].T+M[:3,3])
    if bounds is None:
        allp=np.concatenate(verts); bounds=(allp.min(0),allp.max(0))
    mn,mx=bounds; a1,a2=axes; z=3-a1-a2; k=(S-20)/max(mx[a1]-mn[a1],mx[a2]-mn[a2])
    img=np.zeros((S,S,4),np.uint8); img[:]=(24,24,30,255); zb=np.full((S,S),-1e9)
    for p,Wp in zip(parts,verts):
        mt=p['mat']; di=mt['diffuse'] if mt else None
        tx=tex(di) if (di is not None and di<len(r['tex'])) else None
        if tx is None and only_tex: continue
        uv=p['uv']; X=(Wp[:,a1]-mn[a1])*k+10; Y=S-10-(Wp[:,a2]-mn[a2])*k; Z=Wp[:,z]
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
            else: col=np.array([170,170,170,255],np.uint8)
            img[y0:y1+1,x0:x1+1][upd]=col[upd] if col.ndim==3 else col; sub[upd]=zz[upd]
    return img,bounds
if __name__=='__main__':
    model,clipf,out=sys.argv[1],sys.argv[2],sys.argv[3]; frames=[float(x) for x in sys.argv[4].split(',')]
    clip=an.read_clip(open(clipf,'rb').read())
    b0,bounds=render_frame(model,None,0,only_tex=False)
    ims=[b0]
    for f in frames: im,_=render_frame(model,clip,f,bounds=bounds,only_tex=False); ims.append(im)
    S=ims[0].shape[0]; sheet=Image.new('RGBA',(S*len(ims),S))
    for i,im in enumerate(ims): sheet.paste(Image.fromarray(im,'RGBA'),(i*S,0))
    sheet.save(out); print('saved',out,'frames',frames,'(first panel = bind pose)')
