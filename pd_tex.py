"""Fast texture decoding for Phantom Dust (OG Xbox D3D formats) -> RGBA uint8 (h,w,4), mip level 0 only.
Swizzled formats use Morton interleave of x/y bits (continuing on the longer axis, standard Xbox swizzle)."""
import struct, numpy as np
CODES = {0x00: 'L8', 0x02: 'A1R5G5B5', 0x04: 'A4R4G4B4', 0x05: 'R5G6B5', 0x06: 'A8R8G8B8', 0x07: 'X8R8G8B8', 0x0B: 'P8',
         0x0C: 'DXT1', 0x0E: 'DXT3', 0x0F: 'DXT5', 0x12: 'LIN_A8R8G8B8', 0x13: 'LIN_L8', 0x1E: 'LIN_X8R8G8B8', 0x28: 'V8U8'}
BPP = {'L8': 1, 'A1R5G5B5': 2, 'A4R4G4B4': 2, 'R5G6B5': 2, 'A8R8G8B8': 4, 'X8R8G8B8': 4, 'LIN_A8R8G8B8': 4, 'LIN_L8': 1, 'LIN_X8R8G8B8': 4, 'V8U8': 2}
def morton_index(w, h):
    lw, lh = max(0, w.bit_length() - 1), max(0, h.bit_length() - 1)
    mx = my = 0; ob = 0
    for b in range(max(lw, lh)):
        if b < lw: mx |= 1 << ob; ob += 1
        if b < lh: my |= 1 << ob; ob += 1
    def pdep(v, mask):
        out = np.zeros_like(v); k = 0
        for bit in range(32):
            if mask >> bit & 1:
                out |= ((v >> k) & 1) << bit; k += 1
        return out
    ys, xs = np.mgrid[0:h, 0:w]
    return pdep(xs.astype(np.uint32), mx) | pdep(ys.astype(np.uint32), my)
def c565(c):
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31
    return np.stack([r * 255 // 31, g * 255 // 63, b * 255 // 31], -1).astype(np.int32)
def dxt(buf, w, h, kind):
    bw, bh = max(1, w // 4), max(1, h // 4); bs = 8 if kind == 'DXT1' else 16
    a = np.frombuffer(buf[:bw * bh * bs], np.uint8).reshape(-1, bs)
    cb = a[:, bs - 8:]
    c0 = cb[:, 0].astype(np.uint16) | (cb[:, 1].astype(np.uint16) << 8); c1 = cb[:, 2].astype(np.uint16) | (cb[:, 3].astype(np.uint16) << 8)
    bits = cb[:, 4:8].copy().view('<u4')[:, 0]
    p0, p1 = c565(c0), c565(c1)
    pal = np.zeros((len(a), 4, 4), np.int32); pal[:, :, 3] = 255
    pal[:, 0, :3] = p0; pal[:, 1, :3] = p1
    four = (c0 > c1) | (kind != 'DXT1')
    pal[:, 2, :3] = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    pal[:, 3, :3] = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal[:, 3, 3] = np.where(four, 255, 0)
    idx = ((bits[:, None] >> (2 * np.arange(16))) & 3).astype(np.int32)
    px = pal[np.arange(len(a))[:, None], idx]  # (nb,16,4)
    if kind == 'DXT3':
        av = a[:, :8].copy().view('<u8')[:, 0]
        px[:, :, 3] = (((av[:, None] >> (4 * np.arange(16, dtype=np.uint64))) & 15) * 17).astype(np.int32)
    elif kind == 'DXT5':
        a0, a1 = a[:, 0].astype(np.int32), a[:, 1].astype(np.int32)
        al = np.zeros((len(a), 8), np.int32); al[:, 0] = a0; al[:, 1] = a1
        gt = a0 > a1
        for i in range(6): al[:, 2 + i] = np.where(gt, ((6 - i) * a0 + (1 + i) * a1) // 7, np.where(i < 4, ((4 - i) * a0 + (1 + i) * a1) // 5, 0))
        al[:, 6] = np.where(gt, al[:, 6], 0); al[:, 7] = np.where(gt, al[:, 7], 255)
        ab = np.zeros(len(a), np.uint64)
        for i in range(6): ab |= a[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
        ai = ((ab[:, None] >> (3 * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int32)
        px[:, :, 3] = al[np.arange(len(a))[:, None], ai]
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w].astype(np.uint8)
def decode(buf, code, w, h, size_field=0):
    k = CODES.get(code)
    if k in ('DXT1', 'DXT3', 'DXT5'): return dxt(buf, w, h, k)
    if k is None or k == 'P8': return None
    bpp = BPP[k]
    raw = np.frombuffer(buf[:w * h * bpp], np.uint8)
    if len(raw) < w * h * bpp: return None
    if not k.startswith('LIN'):
        order = morton_index(w, h).reshape(-1)  # pixel (y,x) -> source index
        raw = raw.reshape(-1, bpp)[order]
    else:
        raw = raw.reshape(-1, bpp)
    out = np.zeros((w * h, 4), np.uint8); out[:, 3] = 255
    if k in ('A8R8G8B8', 'LIN_A8R8G8B8', 'X8R8G8B8', 'LIN_X8R8G8B8'):
        out[:, 0], out[:, 1], out[:, 2] = raw[:, 2], raw[:, 1], raw[:, 0]
        if k.endswith('A8R8G8B8'): out[:, 3] = raw[:, 3]
    elif k in ('L8', 'LIN_L8'): out[:, :3] = raw[:, :1]
    elif k == 'V8U8': out[:, 0] = raw[:, 0].astype(np.int8).astype(np.int32) + 128; out[:, 1] = raw[:, 1].astype(np.int8).astype(np.int32) + 128; out[:, 2] = 255
    else:
        v = raw[:, 0].astype(np.uint32) | (raw[:, 1].astype(np.uint32) << 8)
        if k == 'R5G6B5': out[:, 0] = ((v >> 11) & 31) * 255 // 31; out[:, 1] = ((v >> 5) & 63) * 255 // 63; out[:, 2] = (v & 31) * 255 // 31
        elif k == 'A1R5G5B5': out[:, 0] = ((v >> 10) & 31) * 255 // 31; out[:, 1] = ((v >> 5) & 31) * 255 // 31; out[:, 2] = (v & 31) * 255 // 31; out[:, 3] = (v >> 15) * 255
        elif k == 'A4R4G4B4': out[:, 0] = ((v >> 8) & 15) * 17; out[:, 1] = ((v >> 4) & 15) * 17; out[:, 2] = (v & 15) * 17; out[:, 3] = ((v >> 12) & 15) * 17
    return out.reshape(h, w, 4)
