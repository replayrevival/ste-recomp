# SPDX-License-Identifier: GPL-2.0-or-later
# Overlay trigger matrix (0.1.5a): runs overlay_responsive.ps1 for window mode x V-Sync x VRR at 16:9 plus 21:9 for each mode, one game at a time, and writes SUMMARY.csv.
# PASS = the overlay opened, the game window answered messages the whole time (no hung flag) and a WM_CLOSE ended the process (exit code 0).
#   usage: .\overlay_matrix.ps1 -Exe <packaged ste-recomp.exe> -Out <new dir>
param([string]$Exe, [string]$Out)
if (Test-Path $Out) { throw "$Out exists" }
New-Item -ItemType Directory -Force $Out | Out-Null
$runs = @()
foreach ($m in 'window', 'borderless', 'fullscreen') { foreach ($v in 'on', 'off') { foreach ($r in 'on', 'off') { $runs += @{ name = "${m}_vsync${v}_vrr${r}_169"; m = $m; v = $v; r = $r; w = 1920; h = 1080; a = 'wide' } } } }
foreach ($m in 'window', 'borderless', 'fullscreen') { $runs += @{ name = "${m}_vsyncon_vrron_219"; m = $m; v = 'on'; r = 'on'; w = 2560; h = 1080; a = 'wide' } }
"run,opened,max_hung_samples,closed_exit_code,verdict" | Set-Content "$Out\SUMMARY.csv"
foreach ($x in $runs) {
  $d = "$Out\$($x.name)"
  $env_ = @{ STE_VIDEO_MODE = $x.m; STE_VIDEO_VSYNC = $x.v; STE_VIDEO_VRR = $x.r; STE_VIDEO_ASPECT = $x.a }
  & "$PSScriptRoot\overlay_responsive.ps1" -Exe $Exe -Out $d -Width $x.w -Height $x.h -Extra $env_ | Out-Null
  $res = Get-Content "$d\result.txt" -ErrorAction SilentlyContinue
  $opened = ($res -match 'opened=True').Count -gt 0
  $hung = ($res -match 'hungFlag=True').Count
  $code = ($res | Select-String 'exited_after_close=(\w+) code=(\S+)' | ForEach-Object { "$($_.Matches[0].Groups[1].Value)/$($_.Matches[0].Groups[2].Value)" })
  $ok = $opened -and $hung -eq 0 -and $code -eq 'True/0'
  "$($x.name),$opened,$hung,$code,$(if ($ok) { 'PASS' } else { 'FAIL' })" | Add-Content "$Out\SUMMARY.csv"
}
Get-Content "$Out\SUMMARY.csv"
