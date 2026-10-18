Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W2{[DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out R r);
[DllImport("user32.dll")]public static extern bool PrintWindow(IntPtr h,IntPtr hdc,uint f);
public struct R{public int L,T,Rr,B;}}
"@
$p=Get-Process pd_recomp -ErrorAction Stop | Select-Object -First 1
$r=New-Object W2+R; [W2]::GetWindowRect($p.MainWindowHandle,[ref]$r) | Out-Null
$w=$r.Rr-$r.L;$h=$r.B-$r.T
$b=New-Object System.Drawing.Bitmap $w,$h
$g=[System.Drawing.Graphics]::FromImage($b);$hdc=$g.GetHdc()
[W2]::PrintWindow($p.MainWindowHandle,$hdc,2) | Out-Null
$g.ReleaseHdc($hdc);$g.Dispose()
$b.Save($args[0]);$p.MainWindowTitle
