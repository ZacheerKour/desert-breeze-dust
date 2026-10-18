"""Build an Xbox custom soundtrack for Phantom Dust (Options > Music Manager).

  python make_soundtrack.py <audio folder> [--name "My Music"] [--id 1] [--bitrate 128k]

Converts every audio file in the folder to WMA (WMA v2, 44.1 kHz stereo; the
console decoder cannot read newer WMA flavours) with ffmpeg, writes them to
extracted/TDATA/FFFE0000/MUSIC/<id as 4 hex>/<id as 4 hex><song as 4 hex>.WMA, and
(re)writes ST.DB with ONE soundtrack that holds them.

ST.DB layout, recovered from the game's own reader (XAPI, guest code at
0x144858-0x144C75), 512-byte sectors:
  sector 0          header: u32 count, u32 (unused here), u32 soundtrack ids[...]
  sector 1+pos      soundtrack record: u32 0x00021371, u32 id, u32 song_count,
                    u32 song_sector_index[ceil(n/6)] at +0x0C, u32 total_length
                    at +0x15C, wchar name[32] at +0x160
  sector 0x65+idx   song info (6 songs per sector): u32 0x00031073, u32 id,
                    u32 group (song_index // 6), u32 count, u32 song_id[6] at
                    +0x10, u32 length[6] at +0x28, wchar name[6][32] at +0x40
  song file         MUSIC\\<song_id >> 16 as %04x>\\<song_id as %08x>.WMA
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MUSIC = os.path.join(HERE, "extracted", "TDATA", "FFFE0000", "MUSIC")
AUDIO = (".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac", ".wma", ".opus")


def find_ffmpeg():
    p = shutil.which("ffmpeg")
    if p:
        return p
    guess = os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages")
    for root, _dirs, files in os.walk(guess):
        if "ffmpeg.exe" in files:
            return os.path.join(root, "ffmpeg.exe")
    sys.exit("ffmpeg not found on PATH")


def wname(s):
    b = s[:31].encode("utf-16-le")
    return b + b"\x00" * (64 - len(b))


def duration_ms(ffmpeg, path):
    out = subprocess.run([ffmpeg, "-i", path], capture_output=True, text=True).stderr
    for line in out.splitlines():
        if "Duration:" in line:
            h, m, s = line.split("Duration:")[1].split(",")[0].strip().split(":")
            return int((int(h) * 3600 + int(m) * 60 + float(s)) * 1000)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--name", default="Custom Soundtrack")
    ap.add_argument("--id", type=int, default=1)
    ap.add_argument("--bitrate", default="128k")
    a = ap.parse_args()
    ff = find_ffmpeg()
    files = sorted(f for f in os.listdir(a.folder) if f.lower().endswith(AUDIO))
    if not files:
        sys.exit("no audio files in " + a.folder)
    files = files[:84]
    tid = a.id & 0xFFFF
    outdir = os.path.join(MUSIC, "%04x" % tid)
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir, exist_ok=True)

    songs = []
    for i, f in enumerate(files):
        sid = (tid << 16) | (i + 1)
        dst = os.path.join(outdir, "%08x.WMA" % sid)
        src = os.path.join(a.folder, f)
        r = subprocess.run([ff, "-y", "-i", src, "-vn", "-c:a", "wmav2", "-ar", "44100", "-ac", "2",
                            "-b:a", a.bitrate, dst], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("ffmpeg failed on %s:\n%s" % (f, r.stderr[-600:]))
        songs.append((sid, duration_ms(ff, dst), os.path.splitext(f)[0]))
        print("  %-40s -> %s (%d ms)" % (f, os.path.basename(dst), songs[-1][1]))

    n = len(songs)
    groups = (n + 5) // 6
    sectors = [b"\x00" * 512]                      # sector 0 placeholder
    # soundtrack record (sector 1)
    rec = bytearray(512)
    struct.pack_into("<III", rec, 0, 0x00021371, tid, n)
    for g in range(groups):
        struct.pack_into("<I", rec, 0x0C + 4 * g, g)          # song-info sector index
    struct.pack_into("<I", rec, 0x15C, sum(s[1] for s in songs))
    rec[0x160:0x160 + 64] = wname(a.name)
    sectors.append(bytes(rec))
    # pad soundtrack records to 100 sectors so song sectors start at 0x65
    while len(sectors) < 0x65:
        sectors.append(b"\x00" * 512)
    for g in range(groups):
        grp = songs[g * 6:(g + 1) * 6]
        s = bytearray(512)
        struct.pack_into("<IIII", s, 0, 0x00031073, tid, g, len(grp))
        for k, (sid, ln, nm) in enumerate(grp):
            struct.pack_into("<I", s, 0x10 + 4 * k, sid)
            struct.pack_into("<I", s, 0x28 + 4 * k, ln)
            s[0x40 + 64 * k:0x40 + 64 * (k + 1)] = wname(nm)
        sectors.append(bytes(s))
    hdr = bytearray(512)
    struct.pack_into("<II", hdr, 0, 1, tid + 1)
    struct.pack_into("<I", hdr, 8, tid)
    sectors[0] = bytes(hdr)
    os.makedirs(MUSIC, exist_ok=True)
    with open(os.path.join(MUSIC, "ST.DB"), "wb") as fh:
        fh.write(b"".join(sectors))
    print("wrote %s: soundtrack %d \"%s\" with %d songs" % (os.path.join(MUSIC, "ST.DB"), tid, a.name, n))


if __name__ == "__main__":
    main()
