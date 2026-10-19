# interp_cmp_i.py <prefix> <n>: numeric check of dump_<n>_{a,i,b}.bmp: mean abs error i vs avg(a,b), a vs b, i vs a, i vs b (0..255)
import sys,struct
def load(p):
    d=open(p,'rb').read(); off=struct.unpack_from('<I',d,10)[0]; w,h=struct.unpack_from('<ii',d,18); return w,h,d[off:]
pre,n=sys.argv[1],sys.argv[2]
w,h,a=load(f'{pre}_{n}_a.bmp'); _,_,i=load(f'{pre}_{n}_i.bmp'); _,_,b=load(f'{pre}_{n}_b.bmp')
def mae(x,y,step=7):
    s=0;c=0
    for k in range(0,len(x),step): s+=abs(x[k]-y[k]);c+=1
    return s/c
avg=bytes((a[k]+b[k])//2 for k in range(len(a)))
print('a-b %.2f  i-avg(a,b) %.2f  i-a %.2f  i-b %.2f'%(mae(a,b),mae(i,avg),mae(i,a),mae(i,b)))
