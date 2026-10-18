"""Textured model extraction for Phantom Dust .alr: OBJ + MTL + PNG textures, bones applied.
Vertex stream mask (w1 of the 0x16 table entry) = 4-byte slot mask, slots laid out in ascending bit order:
 bits 0-2 position xyz (floats) | bit 3 packed normal (11-11-10, x in low bits) | bit 6 UV0 (2x u16, /4096, may tile >1)
 bit 7 diffuse RGBA | bit 9,10 / 12,13 other packed attrs | bit 20 UV1 (2x u16 /4096)
Material (type 1 entry, 76 B): +0x16 u16 nibble mask (slot j nibble 1 = diffuse, 2 = normal map, 4 = other), +0x18 4x u16 texture indices.
"""
import struct, os, sys, importlib.util, numpy as np
def _m(n):
    sp = importlib.util.spec_from_file_location(n, os.path.join(os.path.dirname(os.path.abspath(__file__)), n + '.py'))
    m = importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
pa, pt, sk = _m('pd_alr'), _m('pd_tex'), _m('pd_skel')
pm = sk.pm
u = lambda b, o: struct.unpack_from('<I', b, o)[0]
def streams(b):
    meta = u(b, 0xC); out = []; o = 0
    while u(b, o) != 0:
        t, s = u(b, o), u(b, o + 4)
        if t == 22:
            for i in range((s - 12) // 28):
                w = struct.unpack_from('<7I', b, o + 12 + 28 * i)
                out.append(dict(stride=(w[0] >> 16) & 0xFF, mask=w[1], count=w[2], off=meta + w[5]))
        o += s
    return out
def read_stream(b, st):
    raw = np.frombuffer(b, np.uint8, st['count'] * st['stride'], st['off']).reshape(st['count'], st['stride'])
    bits = [k for k in range(32) if st['mask'] >> k & 1]
    slot = lambda k: raw[:, 4 * bits.index(k):4 * bits.index(k) + 4].copy().view('<u4')[:, 0] if k in bits else None
    pos = np.stack([raw[:, 4 * i:4 * i + 4].copy().view('<f4')[:, 0] for i in range(3)], 1)
    uv = None
    for k in (6, 20, 10):
        s = slot(k)
        if s is not None:
            scale = 32768.0 if (9 in bits and k == 6) else 4096.0  # masks with bit 9 (0x647/0x64F) store UV as s16/32768
            uv = np.stack([(s & 0xFFFF).astype(np.float32), (s >> 16).astype(np.float32)], 1) / scale; break
    n = slot(3)
    nrm = None
    if n is not None:
        x = (n & 0x7FF).astype(np.int32); y = ((n >> 11) & 0x7FF).astype(np.int32); z = ((n >> 22) & 0x3FF).astype(np.int32)
        x = np.where(x >= 1024, x - 2048, x) / 1023.0; y = np.where(y >= 1024, y - 2048, y) / 1023.0; z = np.where(z >= 512, z - 1024, z) / 511.0
        nrm = np.stack([x, y, z], 1)
    return pos, uv, nrm
def materials(b):
    o = 0
    while u(b, o) != 0:
        t, s = u(b, o), u(b, o + 4)
        if t == 1:
            n = struct.unpack_from('<H', b, o + 8)[0]; res = []
            for i in range(n):
                e = o + 0xC + 0x4C * i; mask = struct.unpack_from('<H', b, e + 0x16)[0]; idx = struct.unpack_from('<4H', b, e + 0x18)
                diff = next((idx[j] for j in range(4) if (mask >> (4 * j)) & 0xF == 1), None)
                nrmt = next((idx[j] for j in range(4) if (mask >> (4 * j)) & 0xF == 2), None)
                res.append(dict(mask=mask, idx=idx, diffuse=diff, normal=nrmt))
            return res
        o += s
    return []
def load_model(p, posed=True):
    b = open(p, 'rb').read(); r = pa.parse(b)
    _, _, meshes = pm.load(p)[0], None, pm.load(p)[2]
    sts = streams(b); S = [read_stream(b, st) for st in sts]
    bones = sk.read_bones(b); W = sk.world(bones) if (bones and posed) else []
    mats = materials(b); out = []
    for m in meshes:
        pos, uv, nrm = S[m['vb']]; bi = struct.unpack_from('<H', b, m['o'] + 0x42)[0]
        out.append(dict(m=m, pos=pos, uv=uv, nrm=nrm, bone=bi, M=(W[bi] if bi < len(W) else None), mat=(mats[m['mat']] if m['mat'] < len(mats) else None)))
    return b, r, out
def tris(m):
    I = m['idx']
    if m['prim'] == 5: T = [(I[j], I[j + 1], I[j + 2]) for j in range(0, len(I) - 2, 3)]
    elif m['prim'] == 6: T = [(I[j], I[j + 1], I[j + 2]) if j % 2 == 0 else (I[j + 1], I[j], I[j + 2]) for j in range(len(I) - 2)]
    else: T = [(I[0], I[j], I[j + 1]) for j in range(1, len(I) - 1)]
    return [t for t in T if t[0] != t[1] and t[1] != t[2] and t[0] != t[2]]
def world_pos(md):
    P = md['pos']
    if md['M'] is None: return P
    M = np.array(md['M'], np.float64); return P @ M[:3, :3].T + M[:3, 3]
def export(p, outdir, posed=True):
    from PIL import Image
    name = os.path.basename(p).split('.')[0]; os.makedirs(os.path.join(outdir, 'tex'), exist_ok=True)
    b, r, parts = load_model(p, posed)
    used = {}
    for pt_ in parts:
        mt = pt_['mat']
        if mt and mt['diffuse'] is not None and mt['diffuse'] < len(r['tex']): used[mt['diffuse']] = True
    for ti in used:
        t = r['tex'][ti]; img = pt.decode(b[r['base'] + t['data']:], t['code'], t['w'], t['h'])
        if img is not None: Image.fromarray(img, 'RGBA').save(os.path.join(outdir, 'tex', '%s_%03d.png' % (name, ti)))
    with open(os.path.join(outdir, name + '.mtl'), 'w') as f:
        for ti in sorted(used): f.write('newmtl tex%d\nKd 1 1 1\nmap_Kd tex/%s_%03d.png\n\n' % (ti, name, ti))
        f.write('newmtl none\nKd 0.7 0.7 0.7\n')
    L = ['mtllib %s.mtl' % name]; vb = 0; nt = 0
    for k, md in enumerate(parts):
        W = world_pos(md); T = tris(md['m'])
        if not T: continue
        usedv = sorted({i for t in T for i in t}); rm = {v: vb + i + 1 for i, v in enumerate(usedv)}
        mt = md['mat']; di = mt['diffuse'] if mt else None
        L.append('o mesh%03d_bone%d' % (k, md['bone'])); L.append('usemtl ' + ('tex%d' % di if di in used else 'none'))
        for v in usedv:
            L.append('v %f %f %f' % tuple(W[v]))
            L.append('vt %f %f' % ((md['uv'][v][0], 1 - md['uv'][v][1]) if md['uv'] is not None else (0, 0)))
        for a, c, d in T: L.append('f %d/%d %d/%d %d/%d' % (rm[a], rm[a], rm[c], rm[c], rm[d], rm[d])); nt += 1
        vb += len(usedv)
    open(os.path.join(outdir, name + '.obj'), 'w').write('\n'.join(L)); return len(parts), nt, len(used)
if __name__ == '__main__':
    print(export(sys.argv[1], sys.argv[2], posed=True))
