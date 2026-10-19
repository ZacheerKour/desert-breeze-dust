import re,json,bisect,sys
t=open(sys.argv[1],encoding='utf-8',errors='replace').read()
i=t.index('[WATCHDOG]')
print(t[i:i+400].split('GS')[0])
vals=[int(m,16) for m in re.findall(r'GS [0-9A-F]+ ([0-9A-F]{8})',t[i:])]
fl=json.load(open('recomp-analysis/disasm_ext/functions.json'))
fl=fl if isinstance(fl,list) else (list(fl['functions'].values()) if isinstance(fl.get('functions'),dict) else fl['functions'])
cv=lambda x:int(x,16) if isinstance(x,str) else x
rows=sorted((cv(f['start']),cv(f['end'])) for f in fl)
st=[r[0] for r in rows]
for k,v in enumerate(vals):
    if 0x11000<=v<0x262000:
        j=bisect.bisect_right(st,v)-1
        if rows[j][0]<=v<=rows[j][1]: print(k,hex(v),'in sub_%08X'%rows[j][0])
