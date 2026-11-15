# SPDX-License-Identifier: GPL-2.0-or-later
# Overlay responsiveness test (0.1.5a): opens the in-game overlay with the scripted Back+Start chord, then checks that the game window keeps
# pumping messages (Process.Responding) and that a close request (WM_CLOSE, what Alt+F4 / the close button send) still ends the process while the overlay is open.
#   usage: .\overlay_responsive.ps1 -Exe <packaged ste-recomp.exe> -Out <new dir> [-Width 1920 -Height 1080] [-Extra @{STE_X='1'}] [-Close $true]
param([string]$Exe, [string]$Out, [int]$Width = 1280, [int]$Height = 720, [hashtable]$Extra = @{}, [bool]$Close = $true, [int]$Hold = 12, [bool]$AltTab = $false, [int]$OpenTimeout = 150,
      [string]$PadBase = "$PSScriptRoot\data\overlay_test2.pad.txt")
Add-Type -TypeDefinition @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class W {
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern int GetClassNameA(IntPtr h, StringBuilder b, int n);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern int GetWindowTextA(IntPtr h, StringBuilder b, int n);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static List<IntPtr> Of(uint pid) { var l = new List<IntPtr>(); EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) l.Add(h); return true; }, IntPtr.Zero); return l; }
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  public static void AltTab() { keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x09, 0, 0, UIntPtr.Zero); keybd_event(0x09, 0, 2, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero); }
  public static string Probe(IntPtr h) { var c = new StringBuilder(128); var t = new StringBuilder(128); GetClassNameA(h, c, 128); GetWindowTextA(h, t, 128); IntPtr r; IntPtr ok = SendMessageTimeout(h, 0, IntPtr.Zero, IntPtr.Zero, 2, 1500, out r);
    return string.Format("hwnd={0:X} class={1} title='{2}' hungFlag={3} WM_NULL_answered={4}", h.ToInt64(), c, t, IsHungAppWindow(h), ok != IntPtr.Zero); }
}
"@
if (Test-Path $Out) { throw "$Out exists" }
New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item $PadBase "$Out\pad.txt"
$e = @{ RECOMP_AC97_READY = '1'; RECOMP_VBLANK = '1'; RECOMP_KERNEL_LOG_BUDGET = '0'; RECOMP_SAVE_DIR = "$env:TEMP\ste_overlay_test_saves"; STE_NO_DIALOG = '1';
        STE_GAME_PATH = $env:STE_GAME_PATH; STE_VIDEO_WIDTH = "$Width"; STE_VIDEO_HEIGHT = "$Height"; STE_VIDEO_CLAMP = '0';
        STE_VIRTUAL_PAD = '1'; STE_PAD_SCRIPT_FILE = "$Out\pad.txt"; STE_OVERLAY_DATA = "$Out\ovdata"; STE_ENHANCE_UNLOCK_ALL_MONSTERS = '1' }
foreach ($k in $Extra.Keys) { $e[$k] = $Extra[$k] }
New-Item -ItemType Directory -Force "$Out\ovdata" | Out-Null
$psi = New-Object Diagnostics.ProcessStartInfo; $psi.FileName = $Exe; $psi.WorkingDirectory = Split-Path $Exe; $psi.UseShellExecute = $false
$psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
foreach ($k in $e.Keys) { $psi.EnvironmentVariables[$k] = $e[$k] }
$p = [Diagnostics.Process]::Start($psi)
$fs = New-Object IO.FileStream("$Out\stderr.txt", [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
$copy = $p.StandardError.BaseStream.CopyToAsync($fs); $null = $p.StandardOutput.ReadToEndAsync()
function Read-Shared($f) { $r = New-Object IO.FileStream($f, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite); $sr = New-Object IO.StreamReader($r); $t = $sr.ReadToEnd(); $sr.Close(); $t }
$series = @(); $sw = [Diagnostics.Stopwatch]::StartNew(); $opened = $false
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $OpenTimeout) { Start-Sleep 1; Add-Content "$Out\hung_series.txt" ("{0}:{1}" -f [int]$sw.Elapsed.TotalSeconds, $(if (-not $p.HasExited) { (([W]::Of([uint32]$p.Id) | ForEach-Object { [W]::IsHungAppWindow($_) }) -join "/") } else { "x" })); $preResp = "see hung_series.txt"; if (((Read-Shared "$Out\stderr.txt") -match '\[OVERLAY\] opened')) { $opened = $true; break } }
$res = @(); $res += "responding_before_open=$preResp"; $res += "opened=$opened after $([int]$sw.Elapsed.TotalSeconds) s"
if ($opened) {
  for ($i = 0; $i -lt $Hold; $i++) { Start-Sleep 1; $p.Refresh(); $res += ("t+{0}s exited={1}" -f ($i + 1), $p.HasExited); if (-not $p.HasExited) { foreach ($h in [W]::Of([uint32]$p.Id)) { $res += ("    " + [W]::Probe($h)) } } }
  if ($AltTab -and -not $p.HasExited) {
    $res += "fg_before=" + [W]::GetForegroundWindow().ToInt64().ToString("X")
    [W]::AltTab(); Start-Sleep 4
    foreach ($h in [W]::Of([uint32]$p.Id)) { $res += ("after_alt_tab: " + [W]::Probe($h)) }
    $res += "fg_after_out=" + [W]::GetForegroundWindow().ToInt64().ToString("X")
    [W]::AltTab(); Start-Sleep 4
    foreach ($h in [W]::Of([uint32]$p.Id)) { $res += ("after_alt_tab_back: " + [W]::Probe($h)) }
    $res += "fg_after_back=" + [W]::GetForegroundWindow().ToInt64().ToString("X") + " exited=" + $p.HasExited
  }
  if ($Close -and -not $p.HasExited) {
    $ok = $false; foreach ($h in [W]::Of([uint32]$p.Id)) { [void][W]::PostMessage($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero); $ok = $true }; $res += "WM_CLOSE posted to visible windows=$ok"
    $p.WaitForExit(10000) | Out-Null
    $res += "exited_after_close=$($p.HasExited) code=$(if ($p.HasExited) { $p.ExitCode } else { 'n/a' })"
  }
}
if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() }
Start-Sleep 1; $fs.Flush(); $fs.Close()
$res | Set-Content "$Out\result.txt"; $res
