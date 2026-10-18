"""Guest-RAM snapshots of the running pd_ab1.exe (host VA == guest VA) for position/HP hunting.
usage: python memtool.py snap <pid> <name>   |   python memtool.py mono <name1> <name2> <name3> <name4>"""
import ctypes, sys, struct, os
from ctypes import wintypes
K=ctypes.WinDLL('kernel32',use_last_error=True)
K.OpenProcess.restype=wintypes.HANDLE
LO,HI=0x10000,0x04000000
def snap(pid,name):
    h=K.OpenProcess(0x10|0x400,False,pid)
    buf=ctypes.create_string_buffer(HI-LO); n=ctypes.c_size_t()
    ok=K.ReadProcessMemory(h,ctypes.c_void_p(LO),buf,HI-LO,ctypes.byref(n))
    open(name+'.snap','wb').write(buf.raw[:n.value]); print('read',n.value,ok)
def load(n): return open(n+'.snap','rb').read()
if __name__=='__main__':
    if sys.argv[1]=='snap': snap(int(sys.argv[2]),sys.argv[3])
