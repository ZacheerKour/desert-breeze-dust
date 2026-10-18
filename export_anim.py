"""Export a skeleton (record type 3) plus a decoded .ala clip as JSON for use in other tools.
Usage: python -I export_anim.py <model.alr> <clip.ala> <out.json>
Angles are Euler radians (assumed order Rz*Ry*Rx), translations in model units, one sample per integer frame."""
import sys, json, importlib.util, numpy as np
def mod(n):
    sp = importlib.util.spec_from_file_location(n, n + '.py'); m = importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m
an, sk = mod('pd_anim'), mod('pd_skel')
model, clipf, out = sys.argv[1:4]
bones = sk.read_bones(open(model, 'rb').read()); clip = an.read_clip(open(clipf, 'rb').read())
n = int(round(clip['duration'])) + 1; frames = []
for f in range(n):
    pb = an.pose(bones, clip, float(f))
    frames.append([dict(t=[round(x, 5) for x in b['t']], r=[round(float(x), 5) for x in b['r']]) for b in pb])
json.dump(dict(note='Phantom Dust clip; angles are Euler radians, assumed Rz*Ry*Rx; frame rate unverified',
               duration_frames=clip['duration'], undecoded_records=clip['undecoded'],
               bones=[dict(index=i, parent=b['parent'], bind_t=list(b['t']), bind_r=[float(x) for x in b['r']], bind_s=list(b['s']), animated=(i in clip['tracks'])) for i, b in enumerate(bones)],
               frames=frames), open(out, 'w'))
print('wrote', out, 'bones', len(bones), 'frames', n, 'animated bones', len(clip['tracks']))
