# Poll window captures; when dark/bright alternation is seen twice in a row, touch presdbg.flag.
import subprocess,time,sys
from PIL import Image
limit=int(sys.argv[1]) if len(sys.argv)>1 else 780
t0=time.time(); streak=0
while time.time()-t0<limit:
    time.sleep(5)
    vals=[]
    for i in range(8):
        f=r"%USERPROFILE%\phantom-dust-re\ex.png"
        r=subprocess.run(["powershell","-NoProfile","-ExecutionPolicy","Bypass","-File","capwin.ps1",f],capture_output=True,text=True)
        if r.returncode: break
        im=Image.open(f).convert("L").resize((130,100)); vals.append(round(sum(im.tobytes())/(130*100)))
    lo=[v for v in vals if v<=12]; hi=[v for v in vals if v>40]
    if len(lo)>=2 and len(hi)>=2:
        streak+=1
        if streak>=2:
            open(r"%USERPROFILE%\phantom-dust-re\pd-game-v2\presdbg.flag","w").write("1"); print("armed at",int(time.time()-t0)); break
    else: streak=0
