import glob,struct,collections
u=lambda b,o:struct.unpack_from('<I',b,o)[0]
files=sorted(glob.glob('extracted/**/*.ala',recursive=True)+glob.glob('out/unpacked/**/*.ala',recursive=True))
print(len(files),'ala files')
types=collections.Counter(); first=collections.Counter(); ok=0; bad=0; nrec=collections.Counter(); tail=collections.Counter()
for p in files:
    b=open(p,'rb').read(); o=0; seq=[]
    while o+8<=len(b) and u(b,o)!=0:
        t,s=u(b,o),u(b,o+4)
        if s<8 or o+s>len(b): bad+=1; break
        seq.append(t); types[t]+=1; o+=s
    else: ok+=1
    first[seq[0] if seq else None]+=1; nrec[len(seq)]+=1
    tail[(len(b)-o)]+=1
print('chains ok',ok,'bad',bad,'| types',dict(types),'| first types',dict(first))
print('records per file (n:files)',sorted(nrec.items())[:12],'| trailing bytes after chain',tail.most_common(5))
p='extracted/Player/enm14a/exmot0.ala'; b=open(p,'rb').read(); o=0
while u(b,o)!=0: print('rec',u(b,o),'size',u(b,o+4),'at',hex(o)); o+=u(b,o+4)
print('file',len(b),'ends at',o)
