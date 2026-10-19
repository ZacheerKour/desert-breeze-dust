"""Skeleton (record type 3) + rigid mesh attachment for Phantom Dust .alr.
Type 3: u32 3,u32 size,u16 n,u8 flags(+10)... entries from +0x10, 64 B each:
  f32 t[3] | f32 rot[3] (Euler radians) | f32 scale[3] | u16 flags,u16 id (w9) | u16 ?,u16 parent (w10; parent 0xFFFF = root) | 4 dwords unknown(hash/garbage)
  then n runtime 4x4 matrices (identity in file) and padding.
Mesh (type 2): u16 at +0x42 = bone index; vertices are bone-local.
"""
import struct, math, sys, importlib.util, os
sp = importlib.util.spec_from_file_location('pd_mesh', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pd_mesh.py'))
pm = importlib.util.module_from_spec(sp); sp.loader.exec_module(pm)
u = lambda b, o: struct.unpack_from('<I', b, o)[0]
def read_bones(b):
    o = 0
    while u(b, o) != 0:
        t, s = u(b, o), u(b, o + 4)
        if t == 3:
            n = struct.unpack_from('<H', b, o + 8)[0]; out = []
            for i in range(n):
                f = struct.unpack_from('<9f', b, o + 0x10 + 64 * i); w = struct.unpack_from('<4I', b, o + 0x10 + 64 * i + 36)
                par = (w[1] >> 16) & 0xFFFF
                out.append(dict(t=f[0:3], r=f[3:6], s=f[6:9], flags=w[0] & 0xFFFF, id=w[0] >> 16, parent=None if par == 0xFFFF else par))
            return out
        o += s
    return []
def mat_mul(a, c): return [[sum(a[i][k] * c[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
def local(b):
    rx, ry, rz = b['r']; sx, sy, sz = b['s']
    cx, sx_ = math.cos(rx), math.sin(rx); cy, sy_ = math.cos(ry), math.sin(ry); cz, sz_ = math.cos(rz), math.sin(rz)
    Rx = [[1, 0, 0, 0], [0, cx, -sx_, 0], [0, sx_, cx, 0], [0, 0, 0, 1]]
    Ry = [[cy, 0, sy_, 0], [0, 1, 0, 0], [-sy_, 0, cy, 0], [0, 0, 0, 1]]
    Rz = [[cz, -sz_, 0, 0], [sz_, cz, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
    R = mat_mul(Rz, mat_mul(Ry, Rx))
    S = [[sx, 0, 0, 0], [0, sy, 0, 0], [0, 0, sz, 0], [0, 0, 0, 1]]
    M = mat_mul(R, S)
    for i in range(3): M[i][3] = b['t'][i]
    return M
def world(bones):
    W = [None] * len(bones)
    def get(i):
        if W[i] is None:
            L = local(bones[i]); p = bones[i]['parent']
            W[i] = L if p is None or p >= len(bones) else mat_mul(get(p), L)
        return W[i]
    for i in range(len(bones)): get(i)
    return W
def xf(M, v): return tuple(M[i][0] * v[0] + M[i][1] * v[1] + M[i][2] * v[2] + M[i][3] for i in range(3))
def export_posed(p, out, posed=True):
    b, streams, meshes = pm.load(p); bones = read_bones(b); W = world(bones) if bones else []
    V = {i: pm.verts(b, st) for i, st in enumerate(streams)}
    lines = []; base = 0; tri = 0
    for k, m in enumerate(meshes):
        bi = struct.unpack_from('<H', b, m['o'] + 0x42)[0]
        M = W[bi] if posed and bi < len(W) else None
        used = sorted(set(m['idx'])); remap = {v: base + i + 1 for i, v in enumerate(used)}
        lines.append('o mesh%03d_bone%d_mat%d' % (k, bi, m['mat']))
        for v in used:
            x = V[m['vb']][v]; x = xf(M, x) if M else x; lines.append('v %f %f %f' % x)
        I = m['idx']
        if m['prim'] == 5: T = [(I[j], I[j + 1], I[j + 2]) for j in range(0, len(I) - 2, 3)]
        elif m['prim'] == 6: T = [(I[j], I[j + 1], I[j + 2]) if j % 2 == 0 else (I[j + 1], I[j], I[j + 2]) for j in range(len(I) - 2)]
        else: T = [(I[0], I[j], I[j + 1]) for j in range(1, len(I) - 1)]
        for a, c, d in T:
            if a == c or c == d or a == d: continue
            lines.append('f %d %d %d' % (remap[a], remap[c], remap[d])); tri += 1
        base += len(used)
    open(out, 'w').write('\n'.join(lines)); return len(bones), len(meshes), tri
if __name__ == '__main__':
    print(export_posed(sys.argv[1], sys.argv[2], posed=(len(sys.argv) < 4 or sys.argv[3] != 'raw')))
