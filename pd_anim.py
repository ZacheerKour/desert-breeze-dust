"""Phantom Dust animation clips (.ala == chain of type-5 records) -> per-bone tracks, evaluated onto a skeleton.
Record (u32 LE): [5][size][f32 duration_frames][u16 target_bone][u16 kind][u32 n_aux][u32 n_rot][u32 0][u32 w7]
 w7 = (aux_elem_bytes << 16) | (channel << 8) | low;  aux track (n_aux keys) at +0x20, then rotation track (n_rot keys).
 kind 7: rotation keys, 7 bytes each = u8 frame + 3 x s16 Euler angles, radians = raw * pi / 16384.
   aux channel 0x00: timed translation (f32 t, x, y, z); 0x04: timed translation X (f32 t, v); 0x05: timed translation Y;
   0x03: constant translation vec3 (2 keys, no time); 0x01: constant vec3 (scale-like).
 kind 8: scalar (t, v) keys. kind 3: vec3 key list + compressed stream (NOT decoded).
"""
import struct, math, numpy as np
u = lambda b, o: struct.unpack_from('<I', b, o)[0]
def read_clip(b):
    o = 0; tracks = {}; undecoded = 0; dur = 0.0
    while o + 8 <= len(b) and u(b, o) != 0:
        s = u(b, o + 4); w = struct.unpack_from('<8I', b, o)
        bone, kind = w[3] & 0xFFFF, w[3] >> 16; n_aux, n_rot, w7 = w[4], w[5], w[7]
        d = struct.unpack('<f', struct.pack('<I', w[2]))[0]; dur = max(dur, d)
        if kind == 7 and (w7 & 0xFF) == 7:
            esz = w7 >> 16; mid = (w7 >> 8) & 0xFF; base = o + 32 + (n_aux * esz if n_aux else 0)
            raw = np.frombuffer(b, np.uint8, 7 * n_rot, base).reshape(n_rot, 7)
            fr = raw[:, 0].astype(np.float64)
            ang = np.stack([raw[:, 1:3].copy().view('<i2')[:, 0], raw[:, 3:5].copy().view('<i2')[:, 0], raw[:, 5:7].copy().view('<i2')[:, 0]], 1).astype(np.float64) * math.pi / 16384.0
            t = tracks.setdefault(bone, {}); t['rot'] = (fr, ang)
            if n_aux:
                if esz == 16: a = np.frombuffer(b, '<f4', 4 * n_aux, o + 32).reshape(n_aux, 4); t['trans'] = (a[:, 0].astype(np.float64), a[:, 1:4].astype(np.float64))
                elif esz == 8:
                    a = np.frombuffer(b, '<f4', 2 * n_aux, o + 32).reshape(n_aux, 2); t.setdefault('axis', {})[{4: 0, 5: 1, 6: 2}.get(mid, mid)] = (a[:, 0].astype(np.float64), a[:, 1].astype(np.float64))
                elif esz == 12:
                    a = np.frombuffer(b, '<f4', 3 * n_aux, o + 32).reshape(n_aux, 3)
                    if mid == 3: t['const_t'] = a[0].astype(np.float64)
                    elif mid == 1: t['const_s'] = a[0].astype(np.float64)
        else: undecoded += 1
        o += s
    return dict(tracks=tracks, duration=dur, undecoded=undecoded)
def interp(fr, vals, f):
    if len(fr) == 1: return vals[0]
    f = min(max(f, fr[0]), fr[-1]); i = int(np.searchsorted(fr, f, side='right') - 1); i = min(i, len(fr) - 2)
    t = (f - fr[i]) / max(fr[i + 1] - fr[i], 1e-9); return vals[i] * (1 - t) + vals[i + 1] * t
def pose(bones, clip, frame):
    out = []
    for i, b in enumerate(bones):
        nb = dict(b); tr = clip['tracks'].get(i)
        if tr:
            if 'rot' in tr: nb['r'] = tuple(interp(tr['rot'][0], tr['rot'][1], frame))
            t = list(b['t'])
            if 'const_t' in tr: t = list(tr['const_t'])
            if 'trans' in tr: t = list(interp(tr['trans'][0], tr['trans'][1], frame))
            for ax, (f, v) in tr.get('axis', {}).items():
                if ax in (0, 1, 2): t[ax] = float(interp(f, v, frame))
            nb['t'] = tuple(t)
        out.append(nb)
    return out
