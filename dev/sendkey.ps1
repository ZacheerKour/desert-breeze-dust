param([int]$vk,[int]$ms=300)
Add-Type @"
using System;using System.Runtime.InteropServices;
public class K{[DllImport("user32.dll")]public static extern void keybd_event(byte b,byte s,uint f,UIntPtr e);
[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")]public static extern bool ShowWindow(IntPtr h,int c);}
"@
$p=Get-Process pd_recomp | Select-Object -First 1
$sh=New-Object -ComObject WScript.Shell
$null=$sh.AppActivate($p.Id)
Start-Sleep -Milliseconds 300
[K]::keybd_event([byte]$vk,0,0,[UIntPtr]::Zero)
Start-Sleep -Milliseconds $ms
[K]::keybd_event([byte]$vk,0,2,[UIntPtr]::Zero)
