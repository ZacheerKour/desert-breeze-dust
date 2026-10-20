#!/usr/bin/env python3
"""Envelope (10 ms RMS) modulation depth per band: audio_env.py dump.wav start end"""
import sys, numpy as np
a=np.fromfile(sys.argv[1],dtype='<i2',offset=44).reshape(-1,2).astype(float).mean(1)
sr=48000; s0=int(float(sys.argv[2])*sr); s1=int(float(sys.argv[3])*sr)
x=a[s0:s1]; win=480; n=len(x)//win
e=np.sqrt((x[:n*win].reshape(n,win)**2).mean(1)); mean=e.mean()
sp=np.abs(np.fft.rfft((e-mean)*np.hanning(n)))*2/ (np.hanning(n).sum())/mean  # modulation depth
fr=np.fft.rfftfreq(n,d=win/sr)
def band(lo,hi): m=(fr>=lo)&(fr<hi); i=np.argmax(sp[m]); return fr[m][i],sp[m][i]
print("mean env %.0f; depth(peak freq, depth) 18-23 Hz: %.1f Hz %.3f | 38-43 Hz: %.1f Hz %.3f | 3-6 Hz: %.1f Hz %.3f | 25-35 Hz: %.1f Hz %.3f"%((mean,)+band(18,23)+band(38,43)+band(3,6)+band(25,35)))
print("total modulation depth 3-60Hz (rms): %.3f"%np.sqrt((sp[(fr>3)&(fr<60)]**2).sum()/2))
