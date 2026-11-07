# SPDX-License-Identifier: GPL-2.0-or-later
# Smoke test: start ste-recomp.exe on your game folder for N seconds and check that it neither crashed nor hit a fatal
# runtime error. Writes the stderr log next to the exe's data folder and prints a short verdict (exit code 0 = pass).
#   .\ste\tests\smoke.ps1 -Exe build\ste-recomp.exe -Game "D:\path\to\extracted" [-Seconds 60] [-Log smoke.err]
# The game window opens during the test. Never run two instances at once.
param([Parameter(Mandatory=$true)][string]$Exe, [Parameter(Mandatory=$true)][string]$Game, [int]$Seconds = 60, [string]$Log = "smoke.err")
$Exe = (Resolve-Path $Exe).Path
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.WorkingDirectory = Split-Path $Exe; $psi.UseShellExecute = $false
$psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
foreach ($k in @($psi.EnvironmentVariables.Keys)) { if ($k -like 'STE_*' -or $k -like 'RECOMP_*' -or $k -like 'XBOX_*') { $psi.EnvironmentVariables.Remove($k) } }
$env0 = @{ RECOMP_AC97_READY = '1'; RECOMP_VBLANK = '1'; RECOMP_KERNEL_LOG_BUDGET = '0'; STE_SHOT_SEC = '0'; STE_PB_LOG_EVERY = '3000'; STE_NO_DIALOG = '1'; STE_GAME_PATH = $Game }
foreach ($k in $env0.Keys) { $psi.EnvironmentVariables[$k] = $env0[$k] }
$p = [Diagnostics.Process]::Start($psi)
$err = $p.StandardError.ReadToEndAsync(); $null = $p.StandardOutput.ReadToEndAsync()
$null = $p.WaitForExit($Seconds * 1000)
$alive = -not $p.HasExited
if ($alive) { $p.Kill(); $p.WaitForExit() }
$null = $err.Wait(5000); $err.Result | Set-Content $Log
$bad = Select-String -Path $Log -Pattern '\[CRASH\]|\[ICALL FAIL\]|\[STUB HIT\]|\[WATCHDOG\]|DEVICE_REMOVED' | ForEach-Object Line
"exe        $Exe"
"sha256     $((Get-FileHash $Exe -Algorithm SHA256).Hash)"
"alive      $alive after $Seconds s" + $(if (-not $alive) { "  exit code 0x{0:X8}" -f $p.ExitCode } else { "" })
"bad lines  $(@($bad).Count)"
$bad | Select-Object -First 10
if ($alive -and -not $bad) { "PASS"; exit 0 } else { "FAIL"; exit 1 }