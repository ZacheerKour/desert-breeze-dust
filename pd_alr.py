"""Phantom Dust .alr/.e resource parser + texture dumper.
Layout (all LE u32):
 0x00 version(0x11) | 0x04 header_size | 0x08 flags | 0x0C meta_size | 0x10 n_objects | 0x14 data_size | 0x18,0x1C 0
 0x20 offsets[n_objects]  (to object records inside meta region)
 @header_size: texture list record: u32 0x15, u32 size, u32 n, n*28 bytes (D3DTexture{Common,Data,Lock,Format,Size} + u64 hash)
 file = meta region (meta_size) + data region (data_size); texture.Data is an offset into the data region.
"""
import struct, sys, os, zlib, collections
FMT = {0x0C: 'DXT1', 0x0E: 'DXT3', 0x0F: 'DXT5', 0x06: 'A8R8G8B8(sw)', 0x07: 'X8R8G8B8(sw)', 0x05: 'R5G6B5(sw)',
       0x04: 'A4R4G4B4(sw)', 0x02: 'A1R5G5B5(sw)', 0x0B: 'P8(sw)', 0x00: 'L8?(sw)', 0x19: 'A8(sw)', 0x1E: 'A8R8G8B8?(lin)',
       0x12: 'A8R8G8B8(lin)', 0x11: 'R5G6B5(lin)', 0x10: 'X8R8G8B8(lin)'}
def parse(b):
    ver, hs, fl, meta, n, dsz = struct.unpack_from('<6I', b, 0)
    offs = struct.unpack_from('<%dI' % n, b, 0x20)
    t, sz, nt = struct.unpack_from('<3I', b, hs)
    assert t == 0x15 and sz == 12 + 28 * nt, (t, sz, nt)
    tex = []
    for i in range(nt):
        common, data, lock, fmt, size, h0, h1 = struct.unpack_from('<7I', b, hs + 12 + 28 * i)
        code = (fmt >> 8) & 0xFF
        w, h = 1 << ((fmt >> 20) & 0xF), 1 << ((fmt >> 24) & 0xF)
        lin = FMT.get(code, '').endswith('(lin)')
        if lin: w, h = (size & 0xFFF) + 1, ((size >> 12) & 0xFFF) + 1
        tex.append(dict(i=i, common=common, data=data, fmt=fmt, code=code, w=w, h=h, mips=(fmt >> 16) & 0xF, hash=(h1 << 32) | h0))
    return dict(ver=ver, hs=hs, meta=meta, n=n, dsz=dsz, offs=offs, tex=tex, base=meta)
def rgb565(c): return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
def dxt(buf, w, h, kind):
    bs = 8 if kind == 'DXT1' else 16
    out = bytearray(w * h * 4); bw = max(1, w // 4)
    for by in range(max(1, h // 4)):
        for bx in range(bw):
            o = (by * bw + bx) * bs
            blk = buf[o:o + bs]
            if len(blk) < bs: continue
            cb = blk[bs - 8:]
            c0, c1, bits = struct.unpack_from('<HHI', cb)
            p0, p1 = rgb565(c0), rgb565(c1)
            if c0 > c1 or kind != 'DXT1':
                pal = [p0 + (255,), p1 + (255,), tuple((2 * a + b2) // 3 for a, b2 in zip(p0, p1)) + (255,), tuple((a + 2 * b2) // 3 for a, b2 in zip(p0, p1)) + (255,)]
            else:
                pal = [p0 + (255,), p1 + (255,), tuple((a + b2) // 2 for a, b2 in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            if kind == 'DXT3':
                av = struct.unpack_from('<Q', blk, 0)[0]
            elif kind == 'DXT5':
                a0, a1 = blk[0], blk[1]
                al = [a0, a1] + ([(( 6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1 else [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
                ai = int.from_bytes(blk[2:8], 'little')
            for py in range(4):
                for px in range(4):
                    k = py * 4 + px; x, y = bx * 4 + px, by * 4 + py
                    if x >= w or y >= h: continue
                    r, g, b2, a = pal[(bits >> (2 * k)) & 3]
                    if kind == 'DXT3': a = ((av >> (4 * k)) & 15) * 17
                    elif kind == 'DXT5': a = al[(ai >> (3 * k)) & 7]
                    q = (y * w + x) * 4; out[q:q + 4] = bytes((r, g, b2, a))
    return bytes(out)
def png(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))
if __name__ == '__main__':
    p = sys.argv[1]; b = open(p, 'rb').read(); r = parse(b)
    print(os.path.basename(p), 'meta', hex(r['meta']), 'data', hex(r['dsz']), 'objects', r['n'], 'textures', len(r['tex']), 'len_ok', len(b) == r['meta'] + r['dsz'])
    if len(sys.argv) > 2:
        os.makedirs(sys.argv[2], exist_ok=True)
        for t in r['tex']:
            k = FMT.get(t['code'])
            print(' tex', t['i'], k or hex(t['code']), '%dx%d' % (t['w'], t['h']), 'mips', t['mips'], 'data', hex(t['data']), 'hash %016x' % t['hash'])
            if k in ('DXT1', 'DXT3', 'DXT5'):
                n = (t['w'] // 4) * (t['h'] // 4) * (8 if k == 'DXT1' else 16)
                png(os.path.join(sys.argv[2], '%03d_%s.png' % (t['i'], k)), t['w'], t['h'], dxt(b[r['base'] + t['data']:r['base'] + t['data'] + n], t['w'], t['h'], k))
