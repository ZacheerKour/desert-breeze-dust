# usage: powershell -File audio_peak.ps1 <pid> [seconds]
# Prints the peak output level (0..1) of the process's audio session, sampled every 100 ms.
param([int]$ProcId, [int]$Secs = 5)
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class AudioPeak {
  [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumeratorCom {}
  [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDeviceEnumerator { int EnumAudioEndpoints(int a, int b, out IntPtr c); int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice dev); }
  [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDevice { int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o); }
  [ComImport, Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioSessionManager2 { int GetAudioSessionControl(IntPtr a, int b, out IntPtr c); int GetSimpleAudioVolume(IntPtr a, int b, out IntPtr c); int GetSessionEnumerator(out IAudioSessionEnumerator e); }
  [ComImport, Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioSessionEnumerator { int GetCount(out int n); int GetSession(int i, out IAudioSessionControl2 s); }
  [ComImport, Guid("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioSessionControl2 {
    int GetState(out int s); int GetDisplayName(out IntPtr s); int SetDisplayName(IntPtr s, IntPtr g); int GetIconPath(out IntPtr s); int SetIconPath(IntPtr s, IntPtr g);
    int GetGroupingParam(out Guid g); int SetGroupingParam(IntPtr o, IntPtr g); int RegisterAudioSessionNotification(IntPtr n); int UnregisterAudioSessionNotification(IntPtr n);
    int GetSessionIdentifier(out IntPtr s); int GetSessionInstanceIdentifier(out IntPtr s); int GetProcessId(out int pid); int IsSystemSoundsSession(); int SetDuckingPreference(int b); }
  [ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioMeterInformation { int GetPeakValue(out float p); }
  public static float Peak(int pid) {
    var en = (IMMDeviceEnumerator)new MMDeviceEnumeratorCom();
    IMMDevice dev; en.GetDefaultAudioEndpoint(0, 1, out dev);
    Guid iid = new Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"); object o; dev.Activate(ref iid, 23, IntPtr.Zero, out o);
    var mgr = (IAudioSessionManager2)o; IAudioSessionEnumerator se; mgr.GetSessionEnumerator(out se);
    int n; se.GetCount(out n); float best = -1;
    for (int i = 0; i < n; i++) {
      IAudioSessionControl2 c; se.GetSession(i, out c); int p; c.GetProcessId(out p);
      if (p == pid) { float v; ((IAudioMeterInformation)c).GetPeakValue(out v); if (v > best) best = v; }
    }
    return best;
  }
}
"@
$max = -1.0; $sum = 0; $n = 0
$end = (Get-Date).AddSeconds($Secs)
while ((Get-Date) -lt $end) { $v = [AudioPeak]::Peak($ProcId); if ($v -gt $max) { $max = $v }; if ($v -ge 0) { $sum += $v; $n++ }; Start-Sleep -Milliseconds 100 }
"pid $ProcId peak max {0:N3} mean {1:N3} samples $n (max -1 = no audio session for that pid)" -f $max, ($(if ($n) { $sum / $n } else { 0 }))
