import json,sys,capstone
XBE=open(r'%USERPROFILE%\phantom-dust-re\extracted\default.xbe','rb').read()
SECS=[(int(x['virtual_addr'],16),x['virtual_size'],int(x['raw_addr'],16),x['raw_size']) for x in json.load(open(r'%USERPROFILE%\phantom-dust-re\recomp-analysis\pd_analysis.json'))['sections']]
MD=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
def rd(va,n):
    for v,vs,r,rs in SECS:
        if v<=va<v+min(vs,rs): return XBE[r+va-v:r+va-v+min(n,v+min(vs,rs)-va)]
    return b''
va=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 64
for i in MD.disasm(rd(va,n),va): print('%08X  %-8s %s'%(i.address,i.mnemonic,i.op_str))
