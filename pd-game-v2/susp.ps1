param([string]$name,[int]$on)
Add-Type @'
using System;using System.Runtime.InteropServices;
public class NtS{[DllImport("ntdll.dll")]public static extern int NtSuspendProcess(IntPtr h);[DllImport("ntdll.dll")]public static extern int NtResumeProcess(IntPtr h);}
'@
foreach($p in Get-Process $name -ErrorAction SilentlyContinue){ if($on){[NtS]::NtSuspendProcess($p.Handle)|Out-Null}else{[NtS]::NtResumeProcess($p.Handle)|Out-Null} }
