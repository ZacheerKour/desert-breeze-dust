import struct,glob,collections
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
files=glob.glob('extracted/**/*.alr',recursive=True)+glob.glob('extracted/**/*.e',recursive=True)
cnt=collections.Counter(); sz=collections.defaultdict(list); ex={}; broken=0; chains=0
for p in files:
    b=open(p,'rb').read()
    ver,hs,fl,meta,n,dsz=struct.unpack_from('<6I',b,0); n&=0xFFFF
    offs=struct.unpack_from('<%dI'%n,b,0x20)
    o=0
    while o+8<=meta and u(b,o)!=0:
        t,s=u(b,o),u(b,o+4)
        if s<8 or o+s>meta: broken+=1; break
        cnt[('top',t)]+=1; sz[('top',t)].append(s); ex.setdefault(('top',t),(p,o)); o+=s
    for r in offs:
        o=r; chains+=1
        while o+8<=meta and u(b,o)!=0:
            t,s=u(b,o),u(b,o+4)
            if s<8 or o+s>meta: broken+=1; break
            cnt[('root',t)]+=1; sz[('root',t)].append(s); ex.setdefault(('root',t),(p,o)); o+=s
print('files',len(files),'root chains',chains,'broken',broken)
for k in sorted(cnt):
    s=sz[k]; print(k,'n=',cnt[k],'size min/med/max',min(s),sorted(s)[len(s)//2],max(s),'e.g.',ex[k][0].split('extracted')[-1],hex(ex[k][1]))
