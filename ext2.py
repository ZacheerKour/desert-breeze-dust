import os
ROOT = os.path.dirname(os.path.abspath(__file__))
import subprocess
from PIL import Image
for i in range(10):
    f=os.path.join(ROOT, 'cap%d.png')%i
    subprocess.run(["powershell","-NoProfile","-ExecutionPolicy","Bypass","-File","capwin.ps1",f],capture_output=True)
    im=Image.open(f).convert("L"); w,h=im.size; px=im.load()
    last=max((y for y in range(40,h) if any(px[x,y]>2 for x in range(300,1000,37))),default=-1)
    print(i,last)
