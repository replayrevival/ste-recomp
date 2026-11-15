# SPDX-License-Identifier: GPL-2.0-or-later
# Regression test of the controller mapping (c377): every face button, Black/White (shoulders), triggers, Start/Back, sticks' clicks and the D-pad,
# pressed one at a time on a virtual SDL Xbox One pad, through the exe's real input path, in the PACKAGED layout (exe copied next to its texturepacks).
#   usage: .\input_selftest.ps1 -Exe <ste-recomp.exe>     (default: artifacts\ste-recomp.exe)
# Cases: 1 built-in defaults must PASS; 2 the launcher's default input_mapping.json must PASS (skipped if the launcher is not on this PC);
#        3 a mapping with A and X swapped must FAIL and log "CUSTOM face-button binding" (proves the test detects the bug it guards against).
param([string]$Exe = ".\ste-recomp.exe", [string]$Pkg = "$env:TEMP\ste_inputtest")
$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force $Pkg | Out-Null
Copy-Item $Exe "$Pkg\ste-recomp.exe" -Force
$env:STE_NO_DIALOG = "1"; $env:STE_GAME_PATH = "$env:STE_GAME_PATH"; $env:STE_INPUT_SELFTEST = "1"
function Run($map) { if ($map) { $env:STE_INPUT_MAP = $map } else { $env:STE_INPUT_MAP = $null }
  $o = & "$Pkg\ste-recomp.exe" 2>&1 | Out-String; return $o }
$fail = 0
$o = Run $null
$r = ($o -split "`n" | Select-String 'INPUTTEST\] RESULT').Line
"case 1 built-in defaults: $r"; if ($r -notmatch 'PASS') { $fail++ }
$lm = "$env:STE_LAUNCHER_MAPPING"
if ($lm -and (Test-Path $lm)) { $o = Run $lm; $r = ($o -split "`n" | Select-String 'INPUTTEST\] RESULT').Line; "case 2 launcher default mapping: $r"; if ($r -notmatch 'PASS') { $fail++ } } else { "case 2 skipped (no launcher mapping file)" }
$sw = Join-Path $PSScriptRoot "data\input_mapping_swapped_ax.json"
$o = Run $sw; $r = ($o -split "`n" | Select-String 'INPUTTEST\] RESULT').Line; $n = ($o -split "`n" | Select-String 'CUSTOM face-button').Count
"case 3 A/X swapped mapping: $r ; custom-binding warning lines: $n"; if ($r -notmatch 'FAIL' -or $n -lt 1) { $fail++ }
$env:STE_INPUT_SELFTEST = $null; $env:STE_INPUT_MAP = $null
if ($fail) { "INPUT SELFTEST: $fail case(s) wrong"; exit 1 } else { "INPUT SELFTEST: all cases as expected"; exit 0 }
