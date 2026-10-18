param([int]$w,[int]$h)
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W{[DllImport("user32.dll")]public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int h2,bool r);}
"@
$p=Get-Process pd_recomp | Select-Object -First 1
[W]::MoveWindow($p.MainWindowHandle,50,50,$w,$h,$true) | Out-Null
