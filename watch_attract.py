# Log window brightness/FPS every 10 s for N seconds.
import subprocess,time,sys
from PIL import Image
n=int(sys.argv[1]); t0=time.time()
while time.time()-t0<n:
    f=r"%USERPROFILE%\phantom-dust-re\ex.png"
    r=subprocess.run(["powershell","-NoProfile","-ExecutionPolicy","Bypass","-File","capwin.ps1",f],capture_output=True,text=True)
    if r.returncode: print("no process"); break
    im=Image.open(f).convert("L").resize((130,100)); print(int(time.time()-t0), round(sum(im.tobytes())/13000), r.stdout.strip()[-26:],flush=True)
    time.sleep(10)
