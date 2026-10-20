#!/usr/bin/env python3
"""Analyse an in-engine audio dump (RECOMP_AUDIO_DUMP=<file.wav>) and its
<file.wav>.events.txt (APU voice events on the same sample timeline).

usage: py -3 dev/audio_analyze.py dump.wav [start_sec end_sec]

Reports: envelope spectrum peaks (flutter), click/discontinuity counts, and
discontinuity statistics at voice events (ON/OFF/EMPTY/LISTDONE/DROP)."""
import sys, os
import numpy as np

def load(path):
    raw = open(path, 'rb').read()[44:]           # header sizes are not finalised
    a = np.frombuffer(raw[:len(raw) // 4 * 4], dtype='<i2').reshape(-1, 2).astype(np.float64)
    return a

def env_spectrum(m, sr=48000, win=480):
    n = len(m) // win
    if n < 256:
        return []
    e = np.sqrt((m[:n * win].reshape(n, win) ** 2).mean(axis=1))
    e = e - e.mean()
    w = np.hanning(n)
    sp = np.abs(np.fft.rfft(e * w))
    fr = np.fft.rfftfreq(n, d=win / sr)
    sel = (fr > 3) & (fr < 60)
    med = np.median(sp[sel]) + 1e-9
    idx = np.argsort(sp[sel])[::-1][:6]
    return [(float(fr[sel][i]), float(sp[sel][i] / med)) for i in idx]

def clicks(m, thr=8.0):
    """Second difference outliers relative to local (2048-sample) RMS of the 2nd difference."""
    d2 = np.abs(m[2:] - 2 * m[1:-1] + m[:-2])
    blk = 2048
    n = len(d2) // blk
    loc = np.sqrt((d2[:n * blk].reshape(n, blk) ** 2).mean(axis=1)) + 1.0
    locs = np.repeat(loc, blk)
    r = d2[:n * blk] / locs
    pos = np.nonzero((r > thr) & (d2[:n * blk] > 600))[0] + 1
    return pos, d2

def main():
    p = sys.argv[1]
    a = load(p)
    sr = 48000
    s0 = int(float(sys.argv[2]) * sr) if len(sys.argv) > 2 else 0
    s1 = int(float(sys.argv[3]) * sr) if len(sys.argv) > 3 else len(a)
    a = a[:s1]
    m = a.mean(axis=1)
    print("samples %d (%.1f s), peak %d, rms %.0f" % (len(a), len(a) / sr, np.abs(a).max(), np.sqrt((m ** 2).mean())))
    seg = m[s0:]
    print("envelope spectrum peaks (Hz, x median):", ["%.1f:%.1f" % t for t in env_spectrum(seg)])
    pos, d2 = clicks(m)
    pos = pos[pos >= s0]
    print("clicks (2nd diff > 8x local & >600): %d  (%.2f /s)" % (len(pos), len(pos) / max(1e-9, (len(m) - s0) / sr)))
    on1024 = np.sum((pos % 1024) < 3)
    print("  of which within 3 samples of a 1024-sample stage boundary: %d (expected by chance %.1f)" % (on1024, len(pos) * 3 / 1024))
    ev = p + ".events.txt"
    if os.path.exists(ev):
        evs = {}
        for ln in open(ev):
            f = ln.split()
            if len(f) < 3:
                continue
            evs.setdefault(f[1], []).append((int(f[0]), f[2]))
        for k, lst in sorted(evs.items()):
            lst = [(t, v) for t, v in lst if t >= s0]
            near = 0
            for t, v in lst:
                lo, hi = max(0, t - 300), min(len(pos) and (len(m)), t + 300)
                if np.any((pos >= lo) & (pos < hi)):
                    near += 1
            by = {}
            for t, v in lst:
                by[v] = by.get(v, 0) + 1
            top = sorted(by.items(), key=lambda x: -x[1])[:5]
            print("  event %-9s n=%d, with a click within +-300 samples: %d   top voices %s" % (k, len(lst), near, top))

main()
