import json,capstone,collections
XBE=open('extracted/default.xbe','rb').read()
SECS=[(int(x['virtual_addr'],16),x['virtual_size'],int(x['raw_addr'],16),x['raw_size']) for x in json.load(open('recomp-analysis/pd_analysis.json'))['sections']]
fl=json.load(open('recomp-analysis/disasm_ext/functions.json'))
fl=fl if isinstance(fl,list) else (list(fl['functions'].values()) if isinstance(fl.get('functions'),dict) else fl['functions'])
cv=lambda x:int(x,16) if isinstance(x,str) else x
MD=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32); MD.detail=True
def rd(va,n):
    for v,vs,r,rs in SECS:
        if v<=va<v+min(vs,rs): return XBE[r+va-v:r+va-v+min(n,v+min(vs,rs)-va)]
    return b''
CF_OF={'jo','jno','jb','jc','jnae','jae','jnb','jnc','ja','jbe','jna','jnbe','seto','setno','setb','setc','setae','setnb','setnc','seta','setbe','adc','sbb','rcl','rcr','into','cmovo','cmovno','cmovb','cmovc','cmovae','cmovnb','cmova','cmovbe','jl','jge','jle','jg','jnl','jng','jnge','jnle'}
FLAGSET={'add','sub','cmp','test','and','or','xor','inc','dec','neg','shl','shr','sar','sal','rol','ror','adc','sbb','bt','bts','btr','btc','mul','imul','div','idiv','popfd','sahf','cmpxchg','xadd','shld','shrd','cmpsb','cmpsd','scasb','scasd','bsf','bsr','lahf'}
seen=set(); out=collections.Counter(); hits=[]
for f in fl:
    a,b=cv(f['start']),cv(f['end'])
    if b-a>20000 or b<=a: continue
    ins=list(MD.disasm(rd(a,b-a+16),a))
    for i,x in enumerate(ins):
        if x.mnemonic in ('mul','imul'):
            key=x.address
            if key in seen: continue
            seen.add(key)
            for y in ins[i+1:i+6]:
                if y.mnemonic in CF_OF:
                    hits.append((x.address,'%s %s'%(x.mnemonic,x.op_str),y.address,y.mnemonic)); break
                if y.mnemonic in FLAGSET or y.mnemonic in ('call','ret','jmp'): break
print('mul/imul sites scanned:',len(seen))
for h in hits: print('%08X %-26s -> %08X %s'%h)
