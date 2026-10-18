import subprocess,sys
from PIL import Image
res=[]
for i in range(12):
    subprocess.run(["powershell","-NoProfile","-ExecutionPolicy","Bypass","-File","capwin.ps1",r"%USERPROFILE%\phantom-dust-re\ex.png"],capture_output=True)
    im=Image.open("ex.png").convert("L"); w,h=im.size
    # last row index where column 640 region non-black
    px=im.load()
    last=max((y for y in range(40,h) if any(px[x,y]>2 for x in range(300,1000,37))),default=-1)
    corner=sum(px[x,y] for x in range(0,50,5) for y in range(40,140,5))
    res.append((last,corner))
print(res)
