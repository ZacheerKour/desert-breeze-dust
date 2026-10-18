# CPU burner: N busy threads at normal priority for S seconds. usage: powershell -File cpuburn.ps1 <threads> <seconds>
param([int]$n=8,[int]$s=600)
$end=(Get-Date).AddSeconds($s)
$jobs=1..$n | ForEach-Object { Start-Job -ScriptBlock { param($e) while((Get-Date) -lt $e){ $x=0; for($i=0;$i -lt 200000;$i++){$x+=$i*$i} } } -ArgumentList $end }
Wait-Job $jobs | Out-Null
