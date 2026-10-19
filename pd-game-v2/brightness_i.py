import glob,sys
exec(open('darkcnt_i.py').read().split("w,h,bpp,rows=readpng")[0])
out=[]
for f in sorted(glob.glob(sys.argv[1]+'/s*.png')):
    try: w,h,bpp,rows=readpng(f)
    except Exception: continue
    s=0;n=0
    for y in range(150,700,25):
        for x in range(80,1200,40):
            p=rows[y][x*bpp:x*bpp+3]; s+=sum(p)/3;n+=1
    out.append(int(s/n))
print(' '.join(map(str,out)))
