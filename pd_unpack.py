"""Phantom Dust (OG Xbox) asset unpackers. Usage: python -I pd_unpack.py <extracted_dir> <out_dir>
.ak : [u32 hdr_size][u32 n][u32 off*n][NUL-terminated names...] -> n .ala animation blobs (each begins u32 5)
.mk : [u32 type][u32 n][u32 data_off][NUL name(s)...] ... -> embedded .alr starting at data_off (begins u32 0x11)
"""
import os, struct, sys
src, dst = sys.argv[1], sys.argv[2]
bad = []
def names_after(b, start, n, limit):
    out, p = [], start
    for _ in range(n):
        e = b.index(b'\0', p); out.append(b[p:e].decode('ascii', 'replace')); p = e + 1
    return out
ak_n = ak_files = mk_files = 0
heads = set()
mk_n = 0
for dp, _, fs in os.walk(src):
    for f in fs:
        p = os.path.join(dp, f); rel = os.path.relpath(p, src)
        ext = os.path.splitext(f)[1].lower()
        if ext == '.ak':
            b = open(p, 'rb').read(); hs, n = struct.unpack_from('<II', b, 0)
            offs = list(struct.unpack_from('<%dI' % n, b, 8)) + [len(b)]
            ok = offs == sorted(offs) and offs[0] == hs and offs[-1] >= offs[-2]
            heads.update(struct.unpack_from('<I', b, o)[0] for o in offs[:-1])
            if not ok: bad.append(('ak', rel)); continue
            names = names_after(b, 8 + 4 * n, n, hs)
            od = os.path.join(dst, os.path.splitext(rel)[0]); os.makedirs(od, exist_ok=True)
            for nm, a, e in zip(names, offs, offs[1:]):
                open(os.path.join(od, os.path.basename(nm)), 'wb').write(b[a:e])
            ak_files += 1; ak_n += n
        elif ext == '.mk':
            b = open(p, 'rb').read(); typ, n, off = struct.unpack_from('<III', b, 0)
            offs = [off] + list(struct.unpack_from('<%dI' % (n - 1), b, 12)) + [len(b)]
            if offs != sorted(offs) or any(struct.unpack_from('<I', b, o)[0] != 0x11 for o in offs[:-1]): bad.append(('mk', rel, hex(typ), n)); continue
            names = names_after(b, 12 + 4 * (n - 1), n, off)
            od = os.path.join(dst, os.path.splitext(rel)[0]); os.makedirs(od, exist_ok=True)
            for nm, a, e in zip(names, offs, offs[1:]):
                open(os.path.join(od, os.path.basename(nm)), 'wb').write(b[a:e])
            mk_files += 1; mk_n += n
print('ak files', ak_files, 'animations', ak_n, 'ala head dwords', sorted(heads), '| mk files', mk_files, 'alr', mk_n, '| failures', len(bad))
for x in bad[:10]: print(' ', x)
