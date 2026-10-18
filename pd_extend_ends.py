"""Extend toolkit function ends to the end of Ghidra's contiguous code range containing the entry.
Reads recomp-analysis/disasm/functions.json + ghidra_funcs.json, writes recomp-analysis/disasm_ext/ (copy) with extended ends."""
import json, shutil, os
import capstone
XBE=open(r'%USERPROFILE%\phantom-dust-re\extracted\default.xbe','rb').read()
SECS=[(int(x['virtual_addr'],16),x['virtual_size'],int(x['raw_addr'],16),x['raw_size']) for x in json.load(open(r'%USERPROFILE%\phantom-dust-re\recomp-analysis\pd_analysis.json'))['sections']]
MD=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
def rd(va,n):
    for v,vs,r,rs in SECS:
        if v<=va<v+min(vs,rs): return XBE[r+va-v:r+va-v+min(n,v+min(vs,rs)-va)]
    return b''
STARTS=set()
def closure_end(start,limit):
    # follow jmp/jcc inside [start,limit) until ret/int3; returns max reachable end
    seen=set(); work=[start]; mx=start
    while work:
        a=work.pop()
        while start<=a<limit and a not in seen:
            ins=next(MD.disasm(rd(a,16),a),None)
            if not ins: break
            seen.add(a); mx=max(mx,a+ins.size)
            m=ins.mnemonic
            if m.startswith('ret') or m=='int3' or m=='hlt': break
            if m.startswith('j') and ins.op_str.startswith('0x'):
                t=int(ins.op_str,16)
                if start<=t<limit and t not in STARTS: work.append(t)
                if m=='jmp': break
            elif m=='jmp': break
            a+=ins.size
    return mx

AN = r'%USERPROFILE%\phantom-dust-re\recomp-analysis'
src = os.path.join(AN, 'disasm'); dst = os.path.join(AN, 'disasm_ext')
if os.path.isdir(dst): shutil.rmtree(dst)
shutil.copytree(src, dst)
F = json.load(open(os.path.join(dst, 'functions.json')))
G = {g['entry']: g for g in json.load(open(os.path.join(AN, 'ghidra_funcs.json')))}
changed = 0; total_bytes = 0
for f in F:
    s = int(f['start'], 16); e = int(f['end'], 16); g = G.get(s)
    if not g: continue
    # contiguous range containing the entry
    rng = next((r for r in g['ranges'] if r[0] <= s <= r[1]), None)
    if not rng: continue
    new_end = rng[1] + 1
    if new_end > e:
        f['end'] = '0x%08X' % new_end
        if 'size' in f: f['size'] = new_end - s
        changed += 1; total_bytes += new_end - e
STARTS.update(int(f['start'],16) for f in F if f.get('detection_method') in ('call_target','prologue'))
ce=0
for f in F:
    s0=int(f['start'],16); e0=int(f['end'],16)
    ne=closure_end(s0,s0+0x3000)
    if ne>e0:
        f['end']='0x%08X'%ne
        if 'size' in f: f['size']=ne-s0
        ce+=1; total_bytes+=ne-e0
print('closure extended',ce)
json.dump(F, open(os.path.join(dst, 'functions.json'), 'w'))
print('extended', changed, 'functions by', total_bytes, 'bytes total')
