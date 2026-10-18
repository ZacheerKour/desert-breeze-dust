import struct,sys,json,bisect
d=open('extracted/default.xbe','rb').read()
base=struct.unpack_from('<I',d,0x104)[0]; nsec=struct.unpack_from('<I',d,0x11C)[0]; sh=struct.unpack_from('<I',d,0x120)[0]-base
secs=[struct.unpack_from('<IIIII',d,sh+i*56) for i in range(nsec)]
fl=json.load(open('recomp-analysis/disasm_ext/functions.json'))
fl=fl if isinstance(fl,list) else (list(fl['functions'].values()) if isinstance(fl.get('functions'),dict) else fl['functions'])
cv=lambda x:int(x,16) if isinstance(x,str) else x
rows=sorted((cv(f['start']),cv(f['end'])) for f in fl); st=[r[0] for r in rows]
for t in [int(x,16) for x in sys.argv[1:]]:
    pat=struct.pack('<I',t)
    for f,va,vs,raw,rs in secs:
        blob=d[raw:raw+rs]; i=-1
        while True:
            i=blob.find(pat,i+1)
            if i<0:break
            a=va+i; j=bisect.bisect_right(st,a)-1
            print(hex(t),'ref at',hex(a),'in sub_%08X'%rows[j][0] if j>=0 and rows[j][0]<=a<rows[j][1] else '(data)')
