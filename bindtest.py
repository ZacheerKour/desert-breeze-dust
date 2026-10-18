import struct,math,numpy as np,importlib.util
sp=importlib.util.spec_from_file_location('pd_skel','pd_skel.py'); sk=importlib.util.module_from_spec(sp); sp.loader.exec_module(sk)
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
mb=open('out/unpacked/Event/1a70/npc05a.alr','rb').read(); bones=sk.read_bones(mb)
ab=open('out/unpacked/Event/1a70/npc05a_u-stand.ala','rb').read(); o=0
rows=[]
while u(ab,o)!=0:
    s=u(ab,o+4); w=struct.unpack_from('<8I',ab,o); a,k=w[3]&0xffff,w[3]>>16
    if k==7 and (w[7]&0xff)==7:
        n_aux,n_rot,esz=w[4],w[5],w[7]>>16; base=o+32+(n_aux*esz if n_aux else 0)
        raw=np.frombuffer(ab,np.uint8,7*n_rot,base).reshape(n_rot,7)
        xyz=np.array([raw[0,1:3].copy().view('<i2')[0],raw[0,3:5].copy().view('<i2')[0],raw[0,5:7].copy().view('<i2')[0]],float)
        rows.append((a,xyz,bones[a]))
    o+=s
print('records',len(rows))
for a,xyz,b in rows[:10]:
    r=np.array(b['r']); print('bone',a,'parent',b['parent'],'bind rot(rad)',np.round(r,3),'| key0 raw',xyz.astype(int),'-> x2pi/65536',np.round(xyz*2*math.pi/65536,3),'| /32768',np.round(xyz/32768,3))
# aggregate: how often does some scale match across all bones?
ratios=[]
for a,xyz,b in rows:
    r=np.array(b['r'])
    for i in range(3):
        if abs(r[i])>0.02 and abs(xyz[i])>20: ratios.append(xyz[i]/r[i])
print('raw/bind ratios (n=%d) median %.1f  IQR %.1f..%.1f   (65536/2pi = %.1f, 32768 = 32768)'%(len(ratios),np.median(ratios),np.percentile(ratios,25),np.percentile(ratios,75),65536/(2*math.pi)))
