# After $1 seconds, poll; when the window is near-black at >100 FPS twice in a row, touch presdbg.flag.
import os
ROOT = os.path.dirname(os.path.abspath(__file__))
import subprocess,time,sys,re
from PIL import Image
start=int(sys.argv[1]); limit=int(sys.argv[2]); t0=time.time(); streak=0
while time.time()-t0<limit:
    time.sleep(8)
    if time.time()-t0<start: continue
    f=os.path.join(ROOT, 'ex.png')
    r=subprocess.run(["powershell","-NoProfile","-ExecutionPolicy","Bypass","-File","capwin.ps1",f],capture_output=True,text=True)
    if r.returncode: break
    im=Image.open(f).convert("L").resize((130,100)); b=sum(im.tobytes())/13000
    m=re.search(r'FPS: ([\d.]+) \| draws: (\d+)',r.stdout)
    fps=float(m.group(1)) if m else 0; dr=int(m.group(2)) if m else -1
    if b<=9 and fps>100 and dr==0:
        streak+=1
        if streak>=2:
            open(os.path.join(ROOT, 'pd-game-v2', 'presdbg.flag'),"w").write("1"); print("armed at",int(time.time()-t0)); break
    else: streak=0
