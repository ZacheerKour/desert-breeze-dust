import multiprocessing as mp,time,os,sys
def burn(t):
    e=time.time()+t
    while time.time()<e: pass
if __name__=='__main__':
    t=float(sys.argv[1])
    ps=[mp.Process(target=burn,args=(t,)) for _ in range(os.cpu_count())]
    [p.start() for p in ps]; [p.join() for p in ps]
    print("burn done",os.cpu_count())
