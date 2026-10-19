param([string]$exe,[string]$text,[int]$ms=110)
Add-Type @"
using System;using System.Runtime.InteropServices;
public class M2{[DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);}
"@
$h=(Get-Process $exe | Select-Object -First 1).MainWindowHandle
function Key($vk,$scan){ [M2]::PostMessage($h,0x100,[IntPtr]$vk,[IntPtr](1 -bor ($scan -shl 16)))|Out-Null; Start-Sleep -Milliseconds $ms; [M2]::PostMessage($h,0x101,[IntPtr]$vk,[IntPtr](0xC0000001 -bor ($scan -shl 16)))|Out-Null; Start-Sleep -Milliseconds 40 }
$i=0
while($i -lt $text.Length){
  $c=$text[$i]
  if($c -eq '{'){ $e=$text.IndexOf('}',$i); $t=$text.Substring($i+1,$e-$i-1); $i=$e+1
    switch($t){ 'BS'{Key 8 14} 'ENTER'{Key 13 28} 'ESC'{Key 27 1} } ; continue }
  $i++
  if($c -ge 'A' -and $c -le 'Z'){ [M2]::PostMessage($h,0x100,[IntPtr]0x10,[IntPtr](1 -bor (0x2A -shl 16)))|Out-Null; Start-Sleep -Milliseconds 30; Key ([int][char]$c) 0; [M2]::PostMessage($h,0x101,[IntPtr]0x10,[IntPtr](0xC0000001 -bor (0x2A -shl 16)))|Out-Null; Start-Sleep -Milliseconds 40 }
  elseif($c -ge 'a' -and $c -le 'z'){ Key ([int][char]$c.ToString().ToUpper()) 0 }
  elseif($c -ge '0' -and $c -le '9'){ Key ([int][char]$c) 0 }
  elseif($c -eq ' '){ Key 32 57 }
}
