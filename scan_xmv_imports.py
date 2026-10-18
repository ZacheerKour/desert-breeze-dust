import os
ROOT = os.path.dirname(os.path.abspath(__file__))
import json,capstone,collections
XBE=open(os.path.join(ROOT, 'extracted', 'default.xbe'),'rb').read()
A=json.load(open(os.path.join(ROOT, 'recomp-analysis', 'pd_analysis.json')))
SECS=[(int(x['virtual_addr'],16),x['virtual_size'],int(x['raw_addr'],16),x['raw_size']) for x in A['sections']]
print([k for k in A.keys()][:12])
MD=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32); MD.detail=True
def rd(va,n):
    for v,vs,r,rs in SECS:
        if v<=va<v+min(vs,rs): return XBE[r+va-v:r+va-v+min(n,v+min(vs,rs)-va)]
    return b''
c=collections.Counter()
lo,hi=0x1FF320,0x228000
for i in MD.disasm(rd(lo,hi-lo),lo):
    if i.mnemonic in('call','jmp') and i.op_str.startswith('dword ptr [0x'):
        c[int(i.op_str.split('0x')[1].rstrip(']'),16)]+=1
for a,n in c.most_common(): print(hex(a),n)
