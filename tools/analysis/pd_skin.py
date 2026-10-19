"""Skinned mesh posing for Phantom Dust.
Mesh record (type 2): index list at +0x68 (ni u16), +0x48 = first vertex (base), padded to 4 bytes, then a skin block:
   [u32 flag][u32 n_palette][n bone-index bytes, padded to 4][n x 4x4 float matrices (identity in file, filled at runtime)]
Vertex slots (mask bits): bit 10 = 2 x u16 register offsets (regA, regB); bit 9 = 4 bytes where byte1 = weight of B, byte2 = weight of A
(byte1 + byte2 == 255). Palette index = (reg - 120) / 3; reg 96 (index -8) means 'no second influence'.
Skinned vertex (model space, bind pose): v' = sum_i w_i * (World_pose[bone_i] * inverse(World_bind[bone_i])) * v.
"""
import struct, numpy as np, importlib.util, os
def _m(n):
    sp = importlib.util.spec_from_file_location(n, os.path.join(os.path.dirname(os.path.abspath(__file__)), n + '.py')); m = importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
md, an = _m('pd_model'), _m('pd_anim'); sk = md.sk
def palette(b, m):
    o = m['o']; s = struct.unpack_from('<I', b, o + 4)[0]; end = (0x68 + 2 * m['ni'] + 3) // 4 * 4
    if end + 8 > s: return None
    n = struct.unpack_from('<I', b, o + end + 4)[0]
    if not (0 < n <= 64) or end + 8 + n > s: return None
    return list(b[o + end + 8:o + end + 8 + n])
def pose_parts(path, clip=None, frame=0.0):
    b, r, parts = md.load_model(path, False); bones = sk.read_bones(b); Wb = [np.array(x) for x in sk.world(bones)]
    Wp = [np.array(x) for x in sk.world(an.pose(bones, clip, frame) if clip else bones)]
    sts = md.streams(b); verts = []; nskin = 0
    for p in parts:
        st = sts[p['m']['vb']]; bits = [k for k in range(32) if st['mask'] >> k & 1]; pal = palette(b, p['m']) if (9 in bits and 10 in bits) else None
        M = Wp[p['bone']] if p['bone'] < len(Wp) else np.eye(4)
        if pal is None:
            P = p['pos']; verts.append(P @ M[:3, :3].T + M[:3, 3]); continue
        raw = np.frombuffer(b, np.uint8, st['count'] * st['stride'], st['off']).reshape(st['count'], st['stride'])
        s9 = raw[:, 4 * bits.index(9):4 * bits.index(9) + 4].astype(np.float64); s10 = raw[:, 4 * bits.index(10):4 * bits.index(10) + 4].copy().view('<u2').astype(int)
        mb = Wb[p['bone']]; P = p['pos'] @ mb[:3, :3].T + mb[:3, 3]  # mesh-bone bind world (identity for the NPC body groups)
        out = P.copy(); base = struct.unpack_from('<I', b, p['m']['o'] + 0x48)[0]; vs = np.array(sorted(set(p['m']['idx']) | set(range(base, base + p['m']['nv']))))
        vs = vs[vs < st['count']]; acc = np.zeros((len(vs), 3)); wsum = np.zeros(len(vs))
        for w, col in ((s9[vs, 2] / 255.0, 0), (s9[vs, 1] / 255.0, 1)):
            k = (s10[vs, col] - 120) // 3
            for j in range(len(vs)):
                if w[j] <= 0 or not (0 <= k[j] < len(pal)) or pal[k[j]] >= len(bones): continue
                T = Wp[pal[k[j]]] @ np.linalg.inv(Wb[pal[k[j]]]); acc[j] += w[j] * (T[:3, :3] @ P[vs[j]] + T[:3, 3]); wsum[j] += w[j]
        ok = wsum > 0; out[vs[ok]] = acc[ok] / wsum[ok][:, None]; out[vs[~ok]] = P[vs[~ok]]; verts.append(out); nskin += 1
    return b, r, parts, verts, nskin
if __name__ == '__main__':
    import sys
    from PIL import Image
    model, outdir, frame = sys.argv[1], sys.argv[2], float(sys.argv[3]); clip = an.read_clip(open(sys.argv[4], 'rb').read()) if len(sys.argv) > 4 else None
    b, r, parts, verts, nskin = pose_parts(model, clip, frame)
    name = os.path.basename(model).split('.')[0]; os.makedirs(os.path.join(outdir, 'tex'), exist_ok=True); used = {}
    for p in parts:
        mt = p['mat']
        if mt and mt['diffuse'] is not None and mt['diffuse'] < len(r['tex']): used[mt['diffuse']] = True
    for ti in used:
        t = r['tex'][ti]; img = md.pt.decode(b[r['base'] + t['data']:], t['code'], t['w'], t['h'])
        if img is not None: Image.fromarray(img, 'RGBA').save(os.path.join(outdir, 'tex', '%s_%03d.png' % (name, ti)))
    open(os.path.join(outdir, name + '.mtl'), 'w').write(''.join('newmtl tex%d\nKd 1 1 1\nmap_Kd tex/%s_%03d.png\n\n' % (ti, name, ti) for ti in sorted(used)) + 'newmtl none\nKd 0.7 0.7 0.7\n')
    L = ['mtllib %s.mtl' % name]; vb = 0; nt = 0
    for k, (p, W) in enumerate(zip(parts, verts)):
        T = md.tris(p['m'])
        if not T: continue
        usedv = sorted({i for t in T for i in t}); rm = {v: vb + i + 1 for i, v in enumerate(usedv)}; di = p['mat']['diffuse'] if p['mat'] else None
        L += ['o mesh%03d_bone%d' % (k, p['bone']), 'usemtl ' + ('tex%d' % di if di in used else 'none')]
        for v in usedv: L += ['v %f %f %f' % tuple(W[v]), 'vt %f %f' % ((p['uv'][v][0], 1 - p['uv'][v][1]) if p['uv'] is not None else (0, 0))]
        for a, c, d in T: L.append('f %d/%d %d/%d %d/%d' % (rm[a], rm[a], rm[c], rm[c], rm[d], rm[d])); nt += 1
        vb += len(usedv)
    open(os.path.join(outdir, name + '.obj'), 'w').write('\n'.join(L)); print('wrote', outdir, 'parts', len(parts), 'skinned parts', nskin, 'triangles', nt)
