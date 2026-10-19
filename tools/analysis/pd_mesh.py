"""Export meshes from a Phantom Dust .alr to OBJ (positions + triangles; UVs only for 32-byte vertices).
Record chain from offset 0: [u32 type][u32 size]... until type 0.
 type 22 (0x16): vertex stream table: u32 22,u32 size,u32 count; entries 28B: [u32 fmt(b2=stride,b0=vtype)][u32][u32 vcount][0][1][u32 data_off][0]
 type 2  : mesh: +0x40 u16 material, +0x44 u16 stream index, +0x46 u16 prim type (D3DPRIMITIVETYPE: 5 list, 6 strip, 7 fan), +0x4C u32 nverts, +0x50 u32 nidx, +0x64 u16 idx[nidx] (absolute in stream)
 data region = file offset meta_size + data_off
"""
import struct, sys, os
u = lambda b, o: struct.unpack_from('<I', b, o)[0]
def load(p):
    b = open(p, 'rb').read(); meta = u(b, 0x0C)
    o = 0; recs = []
    while u(b, o) != 0:
        t, s = u(b, o), u(b, o + 4); recs.append((o, t, s)); o += s
    streams = []
    for o, t, s in recs:
        if t == 22:
            for i in range((s - 12) // 28):
                w = struct.unpack_from('<7I', b, o + 12 + 28 * i)
                streams.append(dict(stride=(w[0] >> 16) & 0xFF, vtype=w[0] & 0xFF, count=w[2], off=meta + w[5]))
    meshes = []
    for o, t, s in recs:
        if t == 2:
            h = struct.unpack_from('<%dI' % 25, b, o)
            ni = h[0x50 // 4]
            meshes.append(dict(o=o, mat=h[0x40 // 4] & 0xFFFF, vb=h[0x44 // 4] & 0xFFFF, prim=h[0x44 // 4] >> 16, nv=h[0x4C // 4], ni=ni,
                               idx=struct.unpack_from('<%dH' % ni, b, o + 0x68)))
    return b, streams, meshes
def verts(b, st):
    return [struct.unpack_from('<3f', b, st['off'] + i * st['stride']) for i in range(st['count'])]
def export(p, out):
    b, streams, meshes = load(p)
    V = {i: verts(b, st) for i, st in enumerate(streams)}
    lines = []; base = 0; tri = 0; bad = 0
    for k, m in enumerate(meshes):
        if m['prim'] not in (5, 6, 7) or m['vb'] >= len(streams): bad += 1; continue
        used = sorted(set(m['idx']))
        if used and used[-1] >= streams[m['vb']]['count']: bad += 1; continue
        remap = {v: base + i + 1 for i, v in enumerate(used)}
        lines.append('o mesh%03d_mat%d_vb%d' % (k, m['mat'], m['vb']))
        for v in used: lines.append('v %f %f %f' % V[m['vb']][v])
        I = m['idx']
        if m['prim'] == 5: tris_ = [(I[j], I[j + 1], I[j + 2]) for j in range(0, len(I) - 2, 3)]
        elif m['prim'] == 6: tris_ = [(I[j], I[j + 1], I[j + 2]) if j % 2 == 0 else (I[j + 1], I[j], I[j + 2]) for j in range(len(I) - 2)]
        else: tris_ = [(I[0], I[j], I[j + 1]) for j in range(1, len(I) - 1)]
        for a, c, d in tris_:
            if a == c or c == d or a == d: continue
            lines.append('f %d %d %d' % (remap[a], remap[c], remap[d])); tri += 1
        base += len(used)
    open(out, 'w').write('\n'.join(lines))
    return len(meshes), tri, bad, V, meshes
if __name__ == '__main__':
    n, tri, bad, V, meshes = export(sys.argv[1], sys.argv[2]); print('meshes', n, 'triangles', tri, 'skipped', bad)
