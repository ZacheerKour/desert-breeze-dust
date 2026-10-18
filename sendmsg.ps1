param([int]$vk,[int]$ms=300)
Add-Type @"
using System;using System.Runtime.InteropServices;
public class M{[DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);}
"@
$p=Get-Process pd_recomp | Select-Object -First 1
$h=$p.MainWindowHandle
[M]::PostMessage($h,0x100,[IntPtr]$vk,[IntPtr]1)|Out-Null
Start-Sleep -Milliseconds $ms
[M]::PostMessage($h,0x101,[IntPtr]$vk,[IntPtr]0xC0000001)|Out-Null
